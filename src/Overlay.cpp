// In-game settings menu for the Event Randomizer, drawn with Dear ImGui over the game's frames.
//
// * Direct3D 11: IDXGISwapChain::Present / Present1 are hooked by patching DXGI's swap-chain vtable
//   (found through a throw-away swap chain), and the menu is drawn into the back buffer.
// * Input: the game window is subclassed so the menu gets keyboard/mouse messages and the game
//   does not; DirectInput 8 GetDeviceState/GetDeviceData are hooked (vtable) so the game reads
//   "nothing pressed" while the menu is open. If the game reads the mouse through DirectInput,
//   the menu pointer follows those mouse movements (the Windows cursor may be locked by the game).
// * The menu edits the shared Settings and saves them to the ini; they apply at the next event.

#include "Shared.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

const GUID kIID_ID3D11Device = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
const GUID kIID_ID3D11Texture2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
const GUID kIID_IDXGISwapChain1 = {0x790a45f7, 0x0d42, 0x4876, {0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa}};
const GUID kIID_IDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kIID_IDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kGUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// ====================================================================== state

CRITICAL_SECTION g_imguiLock;
bool g_imguiReady;
HWND g_hwnd;
WNDPROC g_origWndProc;
ID3D11Device* g_device;  // the device ImGui's DX11 backend was initialised with (not owned)
float g_fontPx = 20.0f;

volatile bool g_menuOpen;
bool g_focusMenu;
ULONGLONG g_blockUntil;      // keep the game's input blocked shortly after closing
ULONGLONG g_toastUntil;
const char* g_toastText;
bool g_firstToastShown;

// DirectInput mouse, accumulated by the hooks while the menu is open
volatile LONG g_diDX, g_diDY, g_diDZ;
volatile LONG g_diButtons;   // bit 0 = left, 1 = right
ULONGLONG g_lastDiMouse;     // last time the game read the mouse through DirectInput
ImVec2 g_virtualMouse;
LONG g_lastButtons;

Settings M;                  // the menu's working copy
bool g_dirty;
ULONGLONG g_dirtySince;

ULONGLONG Now() { return GetTickCount64(); }
bool InputBlocked() { return g_menuOpen || Now() < g_blockUntil; }

void Toast(const char* text, int ms) {
    g_toastText = text;
    g_toastUntil = Now() + ms;
}

void OpenMenu() {
    EnterCriticalSection(&g_settingsLock);
    M = S;
    LeaveCriticalSection(&g_settingsLock);
    g_menuOpen = true;
    g_focusMenu = true;
    g_diDX = g_diDY = g_diDZ = 0;
    g_lastButtons = g_diButtons = 0;
    ImGuiIO& io = ImGui::GetIO();
    g_virtualMouse = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    Log("menu opened");
}

void Commit() {  // menu copy -> shared settings -> ini
    EnterCriticalSection(&g_settingsLock);
    int key = S.menuKey, font = S.fontSize, excl = S.excludeCount;
    uint32_t ex[64];
    memcpy(ex, S.exclude, sizeof(ex));
    S = M;
    S.menuKey = key;
    S.fontSize = font;
    S.excludeCount = excl;
    memcpy(S.exclude, ex, sizeof(ex));
    LeaveCriticalSection(&g_settingsLock);
    SaveSettings();
    g_dirty = false;
}

void CloseMenu() {
    if (g_dirty) Commit();
    g_menuOpen = false;
    g_blockUntil = Now() + 400;
    Toast("Saved - applies when the next event loads", 3500);
    Log("menu closed: Time=%d (%.2f, flow %d, %d-%d) Weather=%d Racers=%d Cops=%d MatchClass=%d", S.timeMode,
        (double)S.fixedHour, S.keepTimeFlow, S.minHour, S.maxHour, S.weatherMode, S.racers, S.cops, S.matchClass);
}

// ====================================================================== the menu

void HourText(int minutes, char* out) { sprintf(out, "%02d:%02d", minutes / 60, minutes % 60); }

void DrawMenu() {
    ImGuiIO& io = ImGui::GetIO();
    const float em = g_fontPx;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(em * 24, 0), ImVec2(em * 40, io.DisplaySize.y * 0.95f));
    if (g_focusMenu) {
        ImGui::SetNextWindowFocus();
        g_focusMenu = false;
    }
    bool open = true;
    char key[16];
    char title[64];
    sprintf(title, "Event Randomizer  (%s / Esc to close)###menu", KeyName(S.menuKey, key));
    ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

    bool changed = false;
    const float col = em * 9.0f;

    // ---------------------------------------------------------------- time of day
    ImGui::SeparatorText("Time of day");
    changed |= ImGui::RadioButton("As the event", &M.timeMode, kTimeDefault);
    ImGui::SameLine(col);
    changed |= ImGui::RadioButton("Random", &M.timeMode, kTimeRandom);
    ImGui::SameLine(col * 2);
    changed |= ImGui::RadioButton("Choose", &M.timeMode, kTimeFixed);

    if (M.timeMode == kTimeFixed) {
        int minutes = (int)(M.fixedHour * 60.0f + 0.5f);
        if (minutes < 0) minutes = 0;
        if (minutes > 1435) minutes = 1435;
        char fmt[16];
        HourText(minutes, fmt);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##hour", &minutes, 0, 1435, fmt, ImGuiSliderFlags_AlwaysClamp)) {
            minutes = (minutes + 2) / 5 * 5;
            M.fixedHour = minutes / 60.0f;
            changed = true;
        }
        struct { const char* name; float hour; } presets[] = {
            {"Dawn", 6.0f}, {"Morning", 9.0f}, {"Noon", 12.0f}, {"Afternoon", 15.5f}, {"Sunset", 19.5f}, {"Night", 23.0f}};
        for (int i = 0; i < 6; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(presets[i].name)) {
                M.fixedHour = presets[i].hour;
                changed = true;
            }
        }
    } else if (M.timeMode == kTimeRandom) {
        char a[16], b[16];
        HourText(M.minHour * 60, a);
        HourText(M.maxHour * 60, b);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Between");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 7);
        if (ImGui::SliderInt("##from", &M.minHour, 0, 23, a, ImGuiSliderFlags_AlwaysClamp)) {
            if (M.maxHour <= M.minHour) M.maxHour = M.minHour + 1;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("and");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 7);
        if (ImGui::SliderInt("##to", &M.maxHour, 1, 24, b, ImGuiSliderFlags_AlwaysClamp)) {
            if (M.minHour >= M.maxHour) M.minHour = M.maxHour - 1;
            changed = true;
        }
    }
    if (M.timeMode != kTimeDefault) {
        bool flow = M.keepTimeFlow != 0;
        if (ImGui::Checkbox("Let the clock run as in the original event", &flow)) {
            M.keepTimeFlow = flow;
            changed = true;
        }
    }

    // ---------------------------------------------------------------- weather
    ImGui::SeparatorText("Weather");
    changed |= ImGui::RadioButton("As the event##w", &M.weatherMode, kWeatherDefault);
    ImGui::SameLine(col);
    changed |= ImGui::RadioButton("Random##w", &M.weatherMode, kWeatherRandom);
    ImGui::SameLine(col * 2);
    changed |= ImGui::RadioButton("Dry", &M.weatherMode, kWeatherDry);
    changed |= ImGui::RadioButton("Wet roads", &M.weatherMode, kWeatherWet);
    ImGui::SameLine(col);
    changed |= ImGui::RadioButton("Rain", &M.weatherMode, kWeatherRain);
    ImGui::SameLine(col * 2);
    changed |= ImGui::RadioButton("Storm", &M.weatherMode, kWeatherStorm);
    if (M.weatherMode == kWeatherRandom) {
        ImGui::TextDisabled("Chances:");
        int* w[] = {&M.wDry, &M.wWet, &M.wRain, &M.wStorm};
        const char* n[] = {"Dry %d", "Wet %d", "Rain %d", "Storm %d"};
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            ImGui::SetNextItemWidth(em * 5.2f);
            char id[8];
            sprintf(id, "##c%d", i);
            changed |= ImGui::SliderInt(id, w[i], 0, 100, n[i], ImGuiSliderFlags_AlwaysClamp);
        }
    }

    // ---------------------------------------------------------------- opponents
    ImGui::SeparatorText("Opponents");
    bool racers = M.racers != 0, cops = M.cops != 0;
    if (ImGui::Checkbox("Swap racers", &racers)) { M.racers = racers; changed = true; }
    ImGui::SameLine(col);
    if (ImGui::Checkbox("Swap cops", &cops)) { M.cops = cops; changed = true; }
    if (M.racers || M.cops) {
        changed |= ImGui::RadioButton("Same class as my car", &M.matchClass, 1);
        ImGui::SameLine(col * 1.6f);
        changed |= ImGui::RadioButton("Keep each opponent's class", &M.matchClass, 0);
        bool dup = M.avoidDuplicates != 0, mine = M.avoidPlayerModel != 0;
        if (ImGui::Checkbox("No duplicate cars", &dup)) { M.avoidDuplicates = dup; changed = true; }
        ImGui::SameLine(col * 1.6f);
        if (ImGui::Checkbox("Never my car's model", &mine)) { M.avoidPlayerModel = mine; changed = true; }
    }

    // ---------------------------------------------------------------- plugin
    ImGui::SeparatorText("Plugin");
    bool en = M.enabled != 0, lo = M.logOnly != 0;
    if (ImGui::Checkbox("Randomizer on", &en)) { M.enabled = en; changed = true; }
    ImGui::SameLine(col * 1.6f);
    if (ImGui::Checkbox("Test mode (log only)", &lo)) { M.logOnly = lo; changed = true; }

    ImGui::Spacing();
    ImGui::TextDisabled("Applies when the next event loads. Saved automatically.");
    ImGui::Spacing();
    if (ImGui::Button("Done", ImVec2(em * 6, 0))) open = false;
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        Settings d;
        DefaultSettings(d);
        M = d;
        changed = true;
    }
    ImGui::End();

    if (changed) {
        if (!g_dirty) g_dirtySince = Now();
        g_dirty = true;
        // keep the shared settings current; the ini is written when the control is released
        EnterCriticalSection(&g_settingsLock);
        Settings keep = S;
        S = M;
        S.menuKey = keep.menuKey;
        S.fontSize = keep.fontSize;
        S.excludeCount = keep.excludeCount;
        memcpy(S.exclude, keep.exclude, sizeof(S.exclude));
        LeaveCriticalSection(&g_settingsLock);
    }
    if (g_dirty && (!ImGui::IsAnyItemActive() || Now() - g_dirtySince > 1500)) Commit();
    if (!open) CloseMenu();
}

void DrawToast() {
    if (!g_toastText || Now() >= g_toastUntil) return;
    ImGui::SetNextWindowPos(ImVec2(g_fontPx, g_fontPx));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(g_toastText);
    ImGui::End();
}

// ====================================================================== window procedure

bool IsInputMessage(UINT m) {
    return (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_INPUT;
}
bool IsMouseMessage(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
bool DiMouseMode() { return Now() - g_lastDiMouse < 1000; }

LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_menuOpen && g_imguiReady) {
        LRESULT r = 0;
        // With a DirectInput mouse the pointer is driven from DirectInput; ignore Windows mouse messages.
        if (!(IsMouseMessage(m) && DiMouseMode())) {
            EnterCriticalSection(&g_imguiLock);
            r = ImGui_ImplWin32_WndProcHandler(h, m, w, l);
            LeaveCriticalSection(&g_imguiLock);
        }
        if (IsInputMessage(m)) return 0;
        if (m == WM_SETCURSOR && r) return r;
    } else if (InputBlocked() && IsInputMessage(m)) {
        return 0;
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

// ====================================================================== DirectInput

typedef HRESULT(WINAPI* GetDeviceState_t)(void* self, DWORD cb, void* data);
typedef HRESULT(WINAPI* GetDeviceData_t)(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags);
typedef HRESULT(WINAPI* GetCapabilities_t)(void* self, void* caps);

struct DiVtable { void** vt; GetDeviceState_t state; GetDeviceData_t data; GetCapabilities_t caps; };
DiVtable g_diVt[2];
int g_diVtCount;

const DiVtable* FindDiVt(void* self) {
    void** vt = *(void***)self;
    for (int i = 0; i < g_diVtCount; ++i)
        if (g_diVt[i].vt == vt) return &g_diVt[i];
    return nullptr;
}

struct DevType { void* dev; DWORD type; };
DevType g_devTypes[16];
int g_devTypeCount;

DWORD DeviceType(void* self, const DiVtable* v) {  // DI8DEVTYPE_* (low byte)
    for (int i = 0; i < g_devTypeCount; ++i)
        if (g_devTypes[i].dev == self) return g_devTypes[i].type;
    DWORD caps[11] = {sizeof(caps)};  // DIDEVCAPS: dwSize, dwFlags, dwDevType, ...
    DWORD type = 0;
    if (v->caps && SUCCEEDED(v->caps(self, caps))) type = caps[2] & 0xFF;
    if (g_devTypeCount < 16) g_devTypes[g_devTypeCount++] = DevType{self, type};
    else g_devTypes[Now() % 16] = DevType{self, type};
    return type;
}
const DWORD kDevMouse = 0x12;  // DI8DEVTYPE_MOUSE

HRESULT WINAPI Hook_GetDeviceState(void* self, DWORD cb, void* data) {
    const DiVtable* v = FindDiVt(self);
    if (!v) return E_FAIL;
    HRESULT hr = v->state(self, cb, data);
    if (FAILED(hr) || !data) return hr;
    DWORD type = DeviceType(self, v);
    if (type == kDevMouse && cb >= 16) {
        g_lastDiMouse = Now();
        if (g_menuOpen) {
            LONG* s = (LONG*)data;
            BYTE* b = (BYTE*)data + 12;
            InterlockedExchangeAdd(&g_diDX, s[0]);
            InterlockedExchangeAdd(&g_diDY, s[1]);
            InterlockedExchangeAdd(&g_diDZ, s[2]);
            g_diButtons = ((b[0] & 0x80) ? 1 : 0) | ((b[1] & 0x80) ? 2 : 0);
        }
    }
    if (InputBlocked()) memset(data, 0, cb);
    return hr;
}

HRESULT WINAPI Hook_GetDeviceData(void* self, DWORD cbObj, void* rgdod, DWORD* inOut, DWORD flags) {
    const DiVtable* v = FindDiVt(self);
    if (!v) return E_FAIL;
    HRESULT hr = v->data(self, cbObj, rgdod, inOut, flags);
    if (FAILED(hr) || !inOut) return hr;
    DWORD type = DeviceType(self, v);
    if (type == kDevMouse) {
        g_lastDiMouse = Now();
        if (g_menuOpen && rgdod && cbObj >= 8) {
            for (DWORD i = 0; i < *inOut; ++i) {
                DWORD* e = (DWORD*)((BYTE*)rgdod + (size_t)i * cbObj);  // dwOfs, dwData
                switch (e[0]) {
                    case 0: InterlockedExchangeAdd(&g_diDX, (LONG)e[1]); break;   // DIMOFS_X
                    case 4: InterlockedExchangeAdd(&g_diDY, (LONG)e[1]); break;   // DIMOFS_Y
                    case 8: InterlockedExchangeAdd(&g_diDZ, (LONG)e[1]); break;   // DIMOFS_Z
                    case 12: g_diButtons = (e[1] & 0x80) ? (g_diButtons | 1) : (g_diButtons & ~1); break;
                    case 13: g_diButtons = (e[1] & 0x80) ? (g_diButtons | 2) : (g_diButtons & ~2); break;
                }
            }
        }
    }
    if (InputBlocked() && rgdod) *inOut = 0;  // events are consumed and dropped
    return hr;
}

void* PatchVtable(void** vt, int index, void* fn) {
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* prev = vt[index];
    vt[index] = fn;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
    return prev;
}

void HookDirectInput() {
    typedef HRESULT(WINAPI * Create_t)(HINSTANCE, DWORD, REFIID, void**, void*);
    HMODULE di = GetModuleHandleW(L"dinput8.dll");
    if (!di) di = LoadLibraryW(L"dinput8.dll");
    Create_t create = di ? (Create_t)GetProcAddress(di, "DirectInput8Create") : nullptr;
    if (!create) { Log("overlay: DirectInput8Create not found - game input is not blocked"); return; }
    const GUID* iids[2] = {&kIID_IDirectInput8W, &kIID_IDirectInput8A};
    for (int k = 0; k < 2; ++k) {
        void* dinput = nullptr;
        if (FAILED(create(GetModuleHandleW(nullptr), 0x0800, *iids[k], &dinput, nullptr)) || !dinput) continue;
        void** divt = *(void***)dinput;
        typedef HRESULT(WINAPI * CreateDevice_t)(void*, REFGUID, void**, void*);
        void* dev = nullptr;
        if (SUCCEEDED(((CreateDevice_t)divt[3])(dinput, kGUID_SysKeyboard, &dev, nullptr)) && dev) {
            void** vt = *(void***)dev;
            bool known = false;
            for (int i = 0; i < g_diVtCount; ++i) known |= g_diVt[i].vt == vt;
            if (!known && g_diVtCount < 2) {
                DiVtable& d = g_diVt[g_diVtCount];
                d.vt = vt;
                d.caps = (GetCapabilities_t)vt[3];
                d.state = (GetDeviceState_t)vt[9];
                d.data = (GetDeviceData_t)vt[10];
                ++g_diVtCount;  // visible before the patch makes the hooks reachable
                PatchVtable(vt, 9, (void*)Hook_GetDeviceState);
                PatchVtable(vt, 10, (void*)Hook_GetDeviceData);
            }
            ((ULONG(WINAPI*)(void*))(*(void***)dev)[2])(dev);  // Release
        }
        ((ULONG(WINAPI*)(void*))divt[2])(dinput);  // Release
    }
    Log("overlay: DirectInput hooked (%d device interface%s)", g_diVtCount, g_diVtCount == 1 ? "" : "s");
}

// ====================================================================== Direct3D 11

typedef HRESULT(WINAPI* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
Present_t g_origPresent;
Present1_t g_origPresent1;

void LoadFont(float px) {
    ImGuiIO& io = ImGui::GetIO();
    wchar_t path[MAX_PATH];
    UINT n = GetWindowsDirectoryW(path, MAX_PATH);
    if (n && n < MAX_PATH - 32) {
        lstrcatW(path, L"\\Fonts\\segoeui.ttf");
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(f, nullptr), got = 0;
            void* data = (size && size < (32u << 20)) ? ImGui::MemAlloc(size) : nullptr;
            bool ok = data && ReadFile(f, data, size, &got, nullptr) && got == size;
            CloseHandle(f);
            if (ok && io.Fonts->AddFontFromMemoryTTF(data, (int)size, px)) return;  // the atlas frees data
            if (data) ImGui::MemFree(data);
        }
    }
    ImFontConfig cfg;
    cfg.SizePixels = px;
    io.Fonts->AddFontDefault(&cfg);
}

bool InitImGui(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) return false;
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(kIID_ID3D11Device, (void**)&dev)) || !dev) return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);

    g_hwnd = desc.OutputWindow;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    float h = (float)(rc.bottom - rc.top);
    if (h < 100) h = (float)desc.BufferDesc.Height;
    g_fontPx = S.fontSize > 0 ? (float)S.fontSize : h * 0.024f;
    if (g_fontPx < 16) g_fontPx = 16;
    if (g_fontPx > 44) g_fontPx = 44;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8;
    st.FrameRounding = 5;
    st.GrabRounding = 5;
    st.WindowBorderSize = 1;
    st.ScaleAllSizes(g_fontPx / 16.0f);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.09f, 0.94f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.22f, 0.45f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.62f, 0.12f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.62f, 0.12f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.75f, 0.30f, 1.0f);
    LoadFont(g_fontPx);

    bool ok = ImGui_ImplWin32_Init(g_hwnd) && ImGui_ImplDX11_Init(dev, ctx);
    g_device = dev;
    ctx->Release();
    dev->Release();  // the backend keeps its own references
    if (!ok) {
        Log("overlay: ImGui initialisation failed");
        return false;
    }
    bool unicode = IsWindowUnicode(g_hwnd) != 0;
    g_origWndProc = (WNDPROC)(unicode ? SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc)
                                      : SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc));
    char k[16];
    Log("overlay: ready on window %p (%dx%d back buffer, font %d px), menu key %s", (void*)g_hwnd,
        (int)desc.BufferDesc.Width, (int)desc.BufferDesc.Height, (int)g_fontPx, KeyName(S.menuKey, k));
    return true;
}

// The swap chain's device changed (e.g. the game recreated it): rebind the DX11 backend.
void RebindDevice(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(kIID_ID3D11Device, (void**)&dev)) || !dev) return;
    if (dev != g_device) {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplDX11_Init(dev, ctx);
        g_device = dev;
        ctx->Release();
        Log("overlay: device changed, renderer rebound");
    }
    dev->Release();
}

void FeedDirectInputMouse() {
    ImGuiIO& io = ImGui::GetIO();
    LONG dx = InterlockedExchange(&g_diDX, 0), dy = InterlockedExchange(&g_diDY, 0), dz = InterlockedExchange(&g_diDZ, 0);
    if (!DiMouseMode()) return;
    float scale = io.DisplaySize.y / 1080.0f;
    if (scale < 1.0f) scale = 1.0f;
    g_virtualMouse.x += dx * scale;
    g_virtualMouse.y += dy * scale;
    if (g_virtualMouse.x < 0) g_virtualMouse.x = 0;
    if (g_virtualMouse.y < 0) g_virtualMouse.y = 0;
    if (g_virtualMouse.x > io.DisplaySize.x - 1) g_virtualMouse.x = io.DisplaySize.x - 1;
    if (g_virtualMouse.y > io.DisplaySize.y - 1) g_virtualMouse.y = io.DisplaySize.y - 1;
    io.AddMousePosEvent(g_virtualMouse.x, g_virtualMouse.y);  // after the backend's own position: wins
    LONG b = g_diButtons;
    for (int i = 0; i < 2; ++i)
        if (((b ^ g_lastButtons) >> i) & 1) io.AddMouseButtonEvent(i, ((b >> i) & 1) != 0);
    g_lastButtons = b;
    if (dz) io.AddMouseWheelEvent(0.0f, dz / 120.0f);
}

DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}

bool g_prevMenuKey, g_prevEsc;

void OnPresent(IDXGISwapChain* sc) {
    EnterCriticalSection(&g_imguiLock);
    if (!g_imguiReady) {
        g_imguiReady = InitImGui(sc);
        if (!g_imguiReady) {
            LeaveCriticalSection(&g_imguiLock);
            return;
        }
    }
    RebindDevice(sc);
    if (!g_firstToastShown) {
        g_firstToastShown = true;
        static char text[96];
        char k[16];
        sprintf(text, "Event Randomizer ready - press %s for options", KeyName(S.menuKey, k));
        Toast(text, 9000);
    }

    // hotkeys (polled: works whatever input API the game uses)
    bool focused = GetForegroundWindow() == g_hwnd;
    bool keyDown = (GetAsyncKeyState(S.menuKey) & 0x8000) != 0;
    bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (focused && keyDown && !g_prevMenuKey) {
        if (g_menuOpen) CloseMenu();
        else OpenMenu();
    } else if (focused && g_menuOpen && escDown && !g_prevEsc) {
        CloseMenu();
    }
    g_prevMenuKey = keyDown;
    g_prevEsc = escDown;
    if (!g_menuOpen && (keyDown || escDown)) g_blockUntil = Now() + 150;  // until released

    bool toast = g_toastText && Now() < g_toastUntil;
    if (!g_menuOpen && !toast) {
        LeaveCriticalSection(&g_imguiLock);
        return;
    }

    ImGui::GetIO().MouseDrawCursor = g_menuOpen;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (g_menuOpen) FeedDirectInputMouse();
    ImGui::NewFrame();
    if (g_menuOpen) DrawMenu();
    DrawToast();
    ImGui::Render();

    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, kIID_ID3D11Texture2D, (void**)&bb)) && bb) {
        D3D11_TEXTURE2D_DESC td;
        bb->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = ViewFormat(td.Format);
        rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11Device* dev = nullptr;
        bb->GetDevice(&dev);
        if (dev && SUCCEEDED(dev->CreateRenderTargetView(bb, &rd, &rtv)) && rtv) {
            ID3D11DeviceContext* ctx = nullptr;
            dev->GetImmediateContext(&ctx);
            ID3D11RenderTargetView* oldRtv = nullptr;
            ID3D11DepthStencilView* oldDsv = nullptr;
            ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
            if (oldRtv) oldRtv->Release();
            if (oldDsv) oldDsv->Release();
            ctx->Release();
            rtv->Release();
        }
        if (dev) dev->Release();
        bb->Release();
    }
    LeaveCriticalSection(&g_imguiLock);
}

HRESULT WINAPI Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
    return g_origPresent(sc, sync, flags);
}

HRESULT WINAPI Hook_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!(flags & DXGI_PRESENT_TEST)) OnPresent(sc);
    return g_origPresent1(sc, sync, flags, p);
}

bool HookD3D() {
    typedef HRESULT(WINAPI * CreateDevSc_t)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                            const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
                                            D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    CreateDevSc_t create = d3d ? (CreateDevSc_t)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : nullptr;
    if (!create) return false;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NFSHPR_EventRandomizer_Dummy";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return false;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = E_FAIL;
    D3D_DRIVER_TYPE types[2] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    for (int i = 0; i < 2 && FAILED(hr); ++i)
        hr = create(nullptr, types[i], nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vt = *(void***)sc;
        g_origPresent = (Present_t)vt[8];
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(kIID_IDXGISwapChain1, (void**)&sc1)) && sc1) {
            void** vt1 = *(void***)sc1;
            g_origPresent1 = (Present1_t)vt1[22];
            PatchVtable(vt1, 22, (void*)Hook_Present1);
            sc1->Release();
        }
        PatchVtable(vt, 8, (void*)Hook_Present);
        ok = true;
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}  // namespace

void Overlay_Start() {
    InitializeCriticalSection(&g_imguiLock);
    HookDirectInput();
    if (HookD3D()) Log("overlay: Direct3D 11 Present hooked, waiting for the first frame");
    else Log("overlay: could not hook Direct3D 11 - the in-game menu is unavailable (ini settings still work)");
}
