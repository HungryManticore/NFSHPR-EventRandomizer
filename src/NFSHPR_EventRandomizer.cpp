// NFSHPR_EventRandomizer.asi - Need for Speed Hot Pursuit Remastered (PC, x64)
//
// Offline events:
//   * AI racers and cops are replaced by random cars of the player's class
//     (Sports/Traffic Police ... Hyper/Special Response; a racer stays a racer, a cop a cop);
//   * time of day and weather: event default, random, or chosen by the player.
// An in-game overlay (Overlay.cpp, default key F5) edits the settings.
//
// How it works: the game's Lua event scripts ask native functions for each competitor's car
// (GetOpponentVehicleId) and pass the event's time/weather to SetTimeOfDay. Those natives are
// 5-byte "jmp" thunks into the protected code; the plugin redirects the jmp to its own function,
// calls the original, and changes the value handed back to Lua (or the arguments passed in).
// StopTraffic, the first step of every event script, marks the start of a new event.
//
// Settings: NFSHPR_EventRandomizer.ini (re-read at every event start, written by the overlay).
// Log:      NFSHPR_EventRandomizer.log

#include "Shared.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "VehicleTable.h"

Settings S;
CRITICAL_SECTION g_settingsLock;
uintptr_t g_base;

namespace {

// ====================================================================== game addresses
// Game build: PE timestamp 613F368C (NFS11Remastered.exe, 459,255,296 bytes).
// Every address is verified before use; on any mismatch the plugin does nothing.

const uint32_t kExpectedTimestamp = 0x613F368C;

struct Binding {
    const char* name;   // Lua global name
    uintptr_t fnRva;    // the jmp thunk registered for it
    uintptr_t regRva;   // "lea r8,[fn]; mov rcx,rbx; lea rdx,[name]; call Register" site
};
const Binding kGetOpponentVehicleId = {"GetOpponentVehicleId", 0x28E700, 0x28DA02};
const Binding kGetSelectedVehicleId = {"GetSelectedVehicleId", 0x298D40, 0x29564E};
const Binding kSetTimeOfDay         = {"SetTimeOfDay",         0x299700, 0x299186};
const Binding kStopTraffic          = {"StopTraffic",          0x298400, 0x2954D8};

struct ApiSig { uintptr_t rva; uint8_t bytes[8]; };
const ApiSig kGetTop     = {0xC68F00, {0x48, 0x8B, 0x41, 0x10, 0x48, 0x2B, 0x41, 0x18}};
const ApiSig kSetTop     = {0xC69C20, {0x85, 0xD2, 0x78, 0x40, 0x4C, 0x63, 0xC2, 0x48}};
const ApiSig kToNumber   = {0xC69EA0, {0x48, 0x83, 0xEC, 0x38, 0xE8, 0x67, 0xE9, 0xFF}};
const ApiSig kPushNumber = {0xC69570, {0x48, 0x8B, 0x41, 0x10, 0xF2, 0x0F, 0x11, 0x08}};
const ApiSig kType       = {0xC69FB0, {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x57, 0xE8, 0xFF}};
const ApiSig kToLString  = {0xC69E00, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74}};
const ApiSig kPushString = {0xC69590, {0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83}};

// IsInOnlineEvent (native, rva 29A7D0) reads [[[base+1279218]+239C8]+F5DA] != 0.
const uintptr_t kRvaGameManager = 0x1279218;
const uintptr_t kOffOnlineState = 0x239C8;
const uintptr_t kOffInOnlineEvent = 0xF5DA;
const ApiSig kIsInOnlineEvent = {0x29A7E8, {0x48, 0x8B, 0x05, 0x29, 0xEA, 0xFD, 0x00, 0x33}};

// ====================================================================== Lua 5.1 (x64) access

struct lua_State;
typedef int (*lua_CFunction)(lua_State*);
typedef int (*Fn_gettop)(lua_State*);
typedef void (*Fn_settop)(lua_State*, int);
typedef double (*Fn_tonumber)(lua_State*, int);
typedef void (*Fn_pushnumber)(lua_State*, double);
typedef int (*Fn_type)(lua_State*, int);
typedef const char* (*Fn_tolstring)(lua_State*, int, size_t*);
typedef void (*Fn_pushstring)(lua_State*, const char*);

Fn_gettop lua_gettop;
Fn_settop lua_settop;
Fn_tonumber lua_tonumber;
Fn_pushnumber lua_pushnumber;
Fn_type lua_type;
Fn_tolstring lua_tolstring;
Fn_pushstring lua_pushstring;

enum { LUA_TNIL = 0, LUA_TBOOLEAN = 1, LUA_TNUMBER = 3, LUA_TSTRING = 4 };

// lua_State: top at +0x10, base at +0x18; TValue = 8-byte value + int type tag at +8 (16 bytes).
uint8_t* ArgSlot(lua_State* L, int i) { return *(uint8_t**)((uint8_t*)L + 0x18) + (size_t)(i - 1) * 16; }
int SlotType(uint8_t* t) { return *(int*)(t + 8); }

lua_CFunction g_origGetOpponentVehicleId, g_origGetSelectedVehicleId, g_origSetTimeOfDay, g_origStopTraffic;

HMODULE g_self;
HANDLE g_log = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_logLock;
wchar_t g_iniPath[MAX_PATH];

bool SafeRead(uintptr_t a, void* dst, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void*)a, dst, n, &got) && got == n;
}

uint64_t g_rng;
uint64_t Rand() {  // xorshift64*
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1DULL;
}
unsigned RandBelow(unsigned n) { return n ? (unsigned)(Rand() % n) : 0; }
double Rand01() { return (double)(int64_t)(Rand() >> 11) * (1.0 / 9007199254740992.0); }

}  // namespace

// ====================================================================== log

void Log(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int o = _snprintf(buf, 32, "[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf + o, sizeof(buf) - 3 - o, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(buf) - 3 - o) n = (int)sizeof(buf) - 3 - o;
    n += o;
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD w;
    EnterCriticalSection(&g_logLock);
    WriteFile(g_log, buf, (DWORD)n, &w, nullptr);
    LeaveCriticalSection(&g_logLock);
}

// ====================================================================== settings

namespace {

const char kDefaultIni[] =
    "; NFSHPR Event Randomizer - re-read at the start of every event.\r\n"
    "; The in-game menu (key in [Overlay]) changes and saves these settings.\r\n"
    "\r\n"
    "[General]\r\n"
    "; 0 = plugin off\r\n"
    "Enabled=1\r\n"
    "; 1 = only write what would change to the log, change nothing in game\r\n"
    "LogOnly=0\r\n"
    "\r\n"
    "[Opponents]\r\n"
    "RandomizeRacers=1\r\n"
    "RandomizeCops=1\r\n"
    "; 1 = opponents and cops come from the class of the player's car\r\n"
    "; 0 = each opponent keeps its own class (only the car changes)\r\n"
    "MatchPlayerClass=1\r\n"
    "; 1 = no two opponents get the same car while the class has enough cars\r\n"
    "AvoidDuplicates=1\r\n"
    "; 1 = opponents never get the same model as the player\r\n"
    "AvoidPlayerModel=0\r\n"
    "; vehicle ids never to pick, comma separated (ids are in the log)\r\n"
    "Exclude=\r\n"
    "\r\n"
    "[TimeOfDay]\r\n"
    "; Default = as the event, Random, Fixed = FixedHour\r\n"
    "Mode=Random\r\n"
    "; hour for Mode=Fixed, 0-24 (12.5 = 12:30)\r\n"
    "FixedHour=12.00\r\n"
    "; 1 = the clock runs as in the original event, 0 = it stands still\r\n"
    "KeepTimeFlow=1\r\n"
    "; start hour range for Mode=Random, 0-24\r\n"
    "MinHour=0\r\n"
    "MaxHour=24\r\n"
    "; 1 = drop the event's fixed sun direction when the time is changed\r\n"
    "ClearSunOverride=1\r\n"
    "\r\n"
    "[Weather]\r\n"
    "; Default = as the event, Random, Dry, Wet, Rain, Storm\r\n"
    "Mode=Random\r\n"
    "; relative chances for Mode=Random\r\n"
    "Dry=40\r\n"
    "Wet=20\r\n"
    "Rain=25\r\n"
    "Storm=15\r\n"
    "\r\n"
    "[Overlay]\r\n"
    "; 0 = no in-game menu (the settings above still apply)\r\n"
    "Enabled=1\r\n"
    "; key that opens the menu: F1-F12, or a virtual-key code number\r\n"
    "Key=F5\r\n"
    "; menu text size in pixels, 0 = automatic\r\n"
    "FontSize=0\r\n";

const char* kTimeModeNames[] = {"Default", "Random", "Fixed"};
const char* kWeatherModeNames[] = {"Default", "Random", "Dry", "Wet", "Rain", "Storm"};

int IniInt(const wchar_t* sec, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, g_iniPath);
}
void IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def, wchar_t* out, int cap) {
    GetPrivateProfileStringW(sec, key, def, out, cap, g_iniPath);
}
void IniWriteInt(const wchar_t* sec, const wchar_t* key, int v) {
    wchar_t b[32];
    _snwprintf(b, 32, L"%d", v);
    b[31] = 0;
    WritePrivateProfileStringW(sec, key, b, g_iniPath);
}
int ModeIndex(const wchar_t* text, const char* const* names, int count, int def) {
    for (int i = 0; i < count; ++i) {
        char a[16];
        WideCharToMultiByte(CP_ACP, 0, text, -1, a, sizeof(a), nullptr, nullptr);
        if (lstrcmpiA(a, names[i]) == 0) return i;
    }
    return def;
}

}  // namespace

void DefaultSettings(Settings& s) {
    memset(&s, 0, sizeof(s));
    s.enabled = 1;
    s.logOnly = 0;
    s.racers = s.cops = s.matchClass = s.avoidDuplicates = 1;
    s.avoidPlayerModel = 0;
    s.timeMode = kTimeRandom;
    s.fixedHour = 12.0f;
    s.keepTimeFlow = 1;
    s.minHour = 0;
    s.maxHour = 24;
    s.clearSunOverride = 1;
    s.weatherMode = kWeatherRandom;
    s.wDry = 40;
    s.wWet = 20;
    s.wRain = 25;
    s.wStorm = 15;
    s.menuKey = VK_F5;
    s.fontSize = 0;
}

const char* KeyName(int vk, char* out) {
    if (vk >= VK_F1 && vk <= VK_F24) sprintf(out, "F%d", vk - VK_F1 + 1);
    else sprintf(out, "key %d", vk);
    return out;
}

void LoadSettings() {
    if (GetFileAttributesW(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(g_iniPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, kDefaultIni, sizeof(kDefaultIni) - 1, &w, nullptr);
            CloseHandle(f);
        }
    }
    Settings s;
    DefaultSettings(s);
    s.enabled = IniInt(L"General", L"Enabled", 1);
    s.logOnly = IniInt(L"General", L"LogOnly", 0);
    s.racers = IniInt(L"Opponents", L"RandomizeRacers", 1);
    s.cops = IniInt(L"Opponents", L"RandomizeCops", 1);
    s.matchClass = IniInt(L"Opponents", L"MatchPlayerClass", 1);
    s.avoidDuplicates = IniInt(L"Opponents", L"AvoidDuplicates", 1);
    s.avoidPlayerModel = IniInt(L"Opponents", L"AvoidPlayerModel", 0);

    wchar_t t[512];
    // Mode keys; the older "Randomize=0/1" keys are honoured when Mode is missing.
    IniStr(L"TimeOfDay", L"Mode", L"", t, 64);
    s.timeMode = t[0] ? ModeIndex(t, kTimeModeNames, 3, kTimeRandom)
                      : (IniInt(L"TimeOfDay", L"Randomize", 1) ? kTimeRandom : kTimeDefault);
    IniStr(L"TimeOfDay", L"FixedHour", L"12", t, 64);
    s.fixedHour = (float)_wtof(t);
    if (s.fixedHour < 0.0f || s.fixedHour > 24.0f) s.fixedHour = 12.0f;
    s.keepTimeFlow = IniInt(L"TimeOfDay", L"KeepTimeFlow", 1);
    s.minHour = IniInt(L"TimeOfDay", L"MinHour", 0);
    s.maxHour = IniInt(L"TimeOfDay", L"MaxHour", 24);
    s.clearSunOverride = IniInt(L"TimeOfDay", L"ClearSunOverride", 1);
    if (s.minHour < 0) s.minHour = 0;
    if (s.maxHour > 24) s.maxHour = 24;
    if (s.maxHour <= s.minHour) { s.minHour = 0; s.maxHour = 24; }

    IniStr(L"Weather", L"Mode", L"", t, 64);
    s.weatherMode = t[0] ? ModeIndex(t, kWeatherModeNames, 6, kWeatherRandom)
                         : (IniInt(L"Weather", L"Randomize", 1) ? kWeatherRandom : kWeatherDefault);
    s.wDry = IniInt(L"Weather", L"Dry", 40);
    s.wWet = IniInt(L"Weather", L"Wet", 20);
    s.wRain = IniInt(L"Weather", L"Rain", 25);
    s.wStorm = IniInt(L"Weather", L"Storm", 15);

    IniStr(L"Overlay", L"Key", L"F5", t, 64);
    if ((t[0] == L'F' || t[0] == L'f') && t[1] >= L'1' && t[1] <= L'9') {
        int n = _wtoi(t + 1);
        s.menuKey = (n >= 1 && n <= 24) ? VK_F1 + n - 1 : VK_F5;
    } else if (t[0] >= L'0' && t[0] <= L'9') {
        s.menuKey = _wtoi(t);
    }
    s.fontSize = IniInt(L"Overlay", L"FontSize", 0);

    IniStr(L"Opponents", L"Exclude", L"", t, 512);
    uint32_t v = 0;
    bool have = false;
    for (const wchar_t* p = t;; ++p) {
        if (*p >= L'0' && *p <= L'9') { v = v * 10 + (uint32_t)(*p - L'0'); have = true; }
        else {
            if (have && s.excludeCount < 64) s.exclude[s.excludeCount++] = v;
            v = 0;
            have = false;
            if (!*p) break;
        }
    }
    EnterCriticalSection(&g_settingsLock);
    S = s;
    LeaveCriticalSection(&g_settingsLock);
}

void SaveSettings() {
    Settings s;
    EnterCriticalSection(&g_settingsLock);
    s = S;
    LeaveCriticalSection(&g_settingsLock);
    wchar_t w[32];
    IniWriteInt(L"General", L"Enabled", s.enabled);
    IniWriteInt(L"General", L"LogOnly", s.logOnly);
    IniWriteInt(L"Opponents", L"RandomizeRacers", s.racers);
    IniWriteInt(L"Opponents", L"RandomizeCops", s.cops);
    IniWriteInt(L"Opponents", L"MatchPlayerClass", s.matchClass);
    IniWriteInt(L"Opponents", L"AvoidDuplicates", s.avoidDuplicates);
    IniWriteInt(L"Opponents", L"AvoidPlayerModel", s.avoidPlayerModel);
    MultiByteToWideChar(CP_ACP, 0, kTimeModeNames[s.timeMode], -1, w, 32);
    WritePrivateProfileStringW(L"TimeOfDay", L"Mode", w, g_iniPath);
    _snwprintf(w, 32, L"%.2f", (double)s.fixedHour);
    w[31] = 0;
    WritePrivateProfileStringW(L"TimeOfDay", L"FixedHour", w, g_iniPath);
    IniWriteInt(L"TimeOfDay", L"KeepTimeFlow", s.keepTimeFlow);
    IniWriteInt(L"TimeOfDay", L"MinHour", s.minHour);
    IniWriteInt(L"TimeOfDay", L"MaxHour", s.maxHour);
    MultiByteToWideChar(CP_ACP, 0, kWeatherModeNames[s.weatherMode], -1, w, 32);
    WritePrivateProfileStringW(L"Weather", L"Mode", w, g_iniPath);
    IniWriteInt(L"Weather", L"Dry", s.wDry);
    IniWriteInt(L"Weather", L"Wet", s.wWet);
    IniWriteInt(L"Weather", L"Rain", s.wRain);
    IniWriteInt(L"Weather", L"Storm", s.wStorm);
    // the old switches are superseded by Mode
    WritePrivateProfileStringW(L"TimeOfDay", L"Randomize", nullptr, g_iniPath);
    WritePrivateProfileStringW(L"Weather", L"Randomize", nullptr, g_iniPath);
}

namespace {

// ====================================================================== vehicles

const char* kRacerClass[] = {"?", "Sports", "Performance", "Super", "Exotic", "Hyper"};
const char* kCopClass[] = {"?", "Traffic Police", "Highway Patrol", "Rapid Deployment", "Speed Enforcement", "Special Response"};

const Vehicle* FindVehicle(uint32_t id) {
    for (int i = 0; i < kVehicleCount; ++i)
        if (kVehicles[i].id == id) return &kVehicles[i];
    return nullptr;
}
const char* ClassName(const Vehicle* v) {
    if (!v || v->tier < 1 || v->tier > 5) return "?";
    return v->side == 3 ? kCopClass[v->tier] : kRacerClass[v->tier];
}
const char* VName(const Vehicle* v) { return v ? v->name : "unknown"; }

bool Excluded(uint32_t id) {
    for (int i = 0; i < S.excludeCount; ++i)
        if (S.exclude[i] == id) return true;
    return false;
}

// ====================================================================== per-event state

struct Slot { int competitor; uint32_t original, chosen; };
struct EventState {
    int number;
    Slot slots[64];
    int slotCount;
    uint32_t playerId;
    bool playerKnown;
};
EventState E;

bool InOnlineEvent() {
    uintptr_t mgr = 0, online = 0;
    uint8_t flag = 0;
    if (!SafeRead(g_base + kRvaGameManager, &mgr, 8) || !mgr) return false;
    if (!SafeRead(mgr + kOffOnlineState, &online, 8) || !online) return false;
    if (!SafeRead(online + kOffInOnlineEvent, &flag, 1)) return false;
    return flag != 0;
}

void NewEvent(const char* why) {
    LoadSettings();
    ++E.number;
    E.slotCount = 0;
    E.playerKnown = false;
    E.playerId = 0;
    Log("---- event %d (%s)  Enabled=%d LogOnly=%d Time=%s Weather=%s", E.number, why, S.enabled, S.logOnly,
        kTimeModeNames[S.timeMode], kWeatherModeNames[S.weatherMode]);
}

// The player's car: GetSelectedVehicleId's original, called on the current Lua state.
// It may clear the Lua stack, so callers must not rely on the stack afterwards.
void LearnPlayerCar(lua_State* L) {
    if (E.playerKnown) return;
    int n = g_origGetSelectedVehicleId(L);
    if (n >= 1 && lua_type(L, -1) == LUA_TNUMBER) {
        E.playerId = (uint32_t)lua_tonumber(L, -1);
        E.playerKnown = true;
        const Vehicle* v = FindVehicle(E.playerId);
        Log("player car: %u %s [%s %s]", E.playerId, VName(v), v ? (v->side == 3 ? "cop" : "racer") : "?", ClassName(v));
    }
    lua_settop(L, 0);
}

bool UsedThisEvent(uint32_t id) {
    for (int i = 0; i < E.slotCount; ++i)
        if (E.slots[i].chosen == id) return true;
    return false;
}

uint32_t PickReplacement(uint32_t original) {
    const Vehicle* o = FindVehicle(original);
    if (!o) return original;  // traffic / unknown vehicle: leave it
    if (o->side == 2 && !S.racers) return original;
    if (o->side == 3 && !S.cops) return original;

    const Vehicle* p = E.playerKnown ? FindVehicle(E.playerId) : nullptr;
    int tier = (S.matchClass && p) ? p->tier : o->tier;

    const Vehicle* pool[64];
    int n = 0;
    for (int pass = 0; pass < 2 && n == 0; ++pass) {  // pass 1 allows duplicates if needed
        for (int i = 0; i < kVehicleCount && n < 64; ++i) {
            const Vehicle* v = &kVehicles[i];
            if (v->side != o->side || v->tier != tier || Excluded(v->id)) continue;
            if (S.avoidPlayerModel && p && lstrcmpA(v->name, p->name) == 0) continue;
            if (pass == 0 && S.avoidDuplicates && UsedThisEvent(v->id)) continue;
            pool[n++] = v;
        }
    }
    if (!n) return original;
    return pool[RandBelow((unsigned)n)]->id;
}

// ====================================================================== hooks

int Hook_StopTraffic(lua_State* L) {
    NewEvent("event loading");
    return g_origStopTraffic(L);
}

// GetOpponentVehicleId(competitor_index) -> vehicle id
int Hook_GetOpponentVehicleId(lua_State* L) {
    int competitor = (lua_gettop(L) >= 1 && lua_type(L, 1) == LUA_TNUMBER) ? (int)lua_tonumber(L, 1) : -1;
    int n = g_origGetOpponentVehicleId(L);
    if (n < 1 || lua_type(L, -1) != LUA_TNUMBER) return n;
    uint32_t original = (uint32_t)lua_tonumber(L, -1);
    if (!S.enabled) return n;
    if (InOnlineEvent()) {
        Log("competitor %d: online event, unchanged", competitor);
        return n;
    }

    // Same competitor and same original car in this event -> same answer (spawn, cutscene, redeploy).
    for (int i = 0; i < E.slotCount; ++i) {
        Slot& s = E.slots[i];
        if (s.competitor == competitor && s.original == original) {
            if (S.logOnly || s.chosen == original) return n;
            lua_settop(L, 0);
            lua_pushnumber(L, (double)s.chosen);
            return 1;
        }
    }

    LearnPlayerCar(L);  // clears the stack
    uint32_t chosen = PickReplacement(original);
    if (E.slotCount < 64) E.slots[E.slotCount++] = Slot{competitor, original, chosen};

    const Vehicle* o = FindVehicle(original);
    const Vehicle* c = FindVehicle(chosen);
    Log("competitor %d: %u %s [%s] -> %u %s [%s]%s", competitor, original, VName(o), ClassName(o), chosen,
        VName(c), ClassName(c), S.logOnly ? "  (log only)" : "");

    lua_settop(L, 0);
    lua_pushnumber(L, (double)(S.logOnly ? original : chosen));
    return 1;
}

// Writes a true/false into argument i, keeping the type the game used for it.
void SetFlagArg(lua_State* L, int i, bool v) {
    if (lua_gettop(L) < i) return;
    uint8_t* t = ArgSlot(L, i);
    switch (SlotType(t)) {
        case LUA_TNUMBER: *(double*)t = v ? 1.0 : 0.0; break;
        case LUA_TSTRING: {
            const char* s = lua_tolstring(L, i, nullptr);
            const char* w = v ? "1" : "0";
            if (s && (s[0] == 't' || s[0] == 'f')) w = v ? "true" : "false";
            if (s && (s[0] == 'T' || s[0] == 'F')) w = v ? "True" : "False";
            lua_pushstring(L, w);
            t = ArgSlot(L, i);  // the stack may have moved
            uint8_t* top = *(uint8_t**)((uint8_t*)L + 0x10) - 16;
            memcpy(t, top, 16);
            lua_settop(L, -2);
            break;
        }
        default:
            *(int*)t = v ? 1 : 0;
            *(int*)(t + 8) = LUA_TBOOLEAN;
            break;
    }
}

void DescribeArg(lua_State* L, int i, char* out, size_t cap) {
    if (lua_gettop(L) < i) { _snprintf(out, cap, "-"); return; }
    uint8_t* t = ArgSlot(L, i);
    switch (SlotType(t)) {
        case LUA_TNIL: _snprintf(out, cap, "nil"); break;
        case LUA_TBOOLEAN: _snprintf(out, cap, "%s", *(int*)t ? "true" : "false"); break;
        case LUA_TNUMBER: _snprintf(out, cap, "%.2f", *(double*)t); break;
        case LUA_TSTRING: {
            const char* s = lua_tolstring(L, i, nullptr);
            _snprintf(out, cap, "\"%.20s\"", s ? s : "");
            break;
        }
        default: _snprintf(out, cap, "type%d", SlotType(t)); break;
    }
    out[cap - 1] = 0;
}

void LogTimeArgs(lua_State* L, const char* label) {
    char a[9][40];
    for (int i = 0; i < 9; ++i) DescribeArg(L, i + 1, a[i], sizeof(a[i]));
    Log("%s start=%s end=%s cycle=%s wet=%s rain=%s thunder=%s altWeather=%s sunOverride=%s sunDir=%s", label,
        a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
}

double WrapHour(double h) {
    while (h >= 24.0) h -= 24.0;
    while (h < 0.0) h += 24.0;
    return h;
}

// SetTimeOfDay(start, finish, cycle, isWet, isRain, isThunder, isAlternativeWeather, overrideSun, sunDir)
int Hook_SetTimeOfDay(lua_State* L) {
    LogTimeArgs(L, "event time/weather:");
    if (!S.enabled || InOnlineEvent()) return g_origSetTimeOfDay(L);
    if (S.logOnly) {
        Log("(log only: time and weather unchanged)");
        return g_origSetTimeOfDay(L);
    }
    int top = lua_gettop(L);
    if (S.timeMode != kTimeDefault && top >= 2 && SlotType(ArgSlot(L, 1)) == LUA_TNUMBER) {
        double start = *(double*)ArgSlot(L, 1);
        bool finishIsNumber = SlotType(ArgSlot(L, 2)) == LUA_TNUMBER;
        double span = finishIsNumber ? *(double*)ArgSlot(L, 2) - start : 0.0;
        double ns = S.timeMode == kTimeFixed ? (double)S.fixedHour
                                             : S.minHour + Rand01() * (S.maxHour - S.minHour);
        ns = WrapHour(ns);
        *(double*)ArgSlot(L, 1) = ns;
        if (finishIsNumber) *(double*)ArgSlot(L, 2) = S.keepTimeFlow ? WrapHour(ns + span) : ns;
        if (S.clearSunOverride && top >= 8) SetFlagArg(L, 8, false);
    }
    if (S.weatherMode != kWeatherDefault && top >= 6) {
        int w = S.weatherMode;
        if (w == kWeatherRandom) {
            int total = S.wDry + S.wWet + S.wRain + S.wStorm;
            int r = total > 0 ? (int)RandBelow((unsigned)total) : 0;
            w = total <= 0 ? kWeatherDry
              : r < S.wDry ? kWeatherDry
              : r < S.wDry + S.wWet ? kWeatherWet
              : r < S.wDry + S.wWet + S.wRain ? kWeatherRain : kWeatherStorm;
        }
        SetFlagArg(L, 4, w != kWeatherDry);
        SetFlagArg(L, 5, w == kWeatherRain || w == kWeatherStorm);
        SetFlagArg(L, 6, w == kWeatherStorm);
    }
    LogTimeArgs(L, "new time/weather:  ");
    return g_origSetTimeOfDay(L);
}

// ====================================================================== install

bool CheckBytes(uintptr_t rva, const uint8_t* expect, size_t n) {
    uint8_t b[16];
    return n <= sizeof(b) && SafeRead(g_base + rva, b, n) && memcmp(b, expect, n) == 0;
}

// The registration site must load this function and this exact name, and the function must be a jmp.
bool CheckBinding(const Binding& b) {
    uint8_t s[22];
    if (!SafeRead(g_base + b.regRva, s, sizeof(s))) return false;
    if (s[0] != 0x4C || s[1] != 0x8D || s[2] != 0x05 || s[10] != 0x48 || s[11] != 0x8D || s[12] != 0x15) return false;
    int32_t d1, d2;
    memcpy(&d1, s + 3, 4);
    memcpy(&d2, s + 13, 4);
    uintptr_t fn = b.regRva + 7 + (intptr_t)d1;
    uintptr_t nameRva = b.regRva + 17 + (intptr_t)d2;
    if (fn != b.fnRva) return false;
    char name[40] = {};
    size_t len = strlen(b.name) + 1;
    if (!SafeRead(g_base + nameRva, name, len) || memcmp(name, b.name, len) != 0) return false;
    uint8_t op;
    return SafeRead(g_base + b.fnRva, &op, 1) && op == 0xE9;
}

uint8_t* g_tramp;  // one page within +-2 GB of the thunks
int g_trampUsed;

bool AllocTrampolines() {
    uintptr_t target = g_base + 0x290000;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = (g_base & ~(uintptr_t)0xFFFF) - 0x10000; a > 0x10000 && target - a < 0x70000000; a -= 0x10000) {
        if (VirtualQuery((void*)a, &mbi, sizeof(mbi)) != sizeof(mbi)) continue;
        if (mbi.State != MEM_FREE) continue;
        void* p = VirtualAlloc((void*)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) { g_tramp = (uint8_t*)p; return true; }
    }
    return false;
}

// Redirects the thunk's jmp to `detour`; returns the original jump target.
lua_CFunction HookThunk(const Binding& b, lua_CFunction detour) {
    uint8_t* thunk = (uint8_t*)(g_base + b.fnRva);
    int32_t rel;
    memcpy(&rel, thunk + 1, 4);
    lua_CFunction original = (lua_CFunction)(thunk + 5 + rel);

    uint8_t* t = g_tramp + g_trampUsed;  // jmp [rip+0]; dq detour
    g_trampUsed += 16;
    t[0] = 0xFF;
    t[1] = 0x25;
    memset(t + 2, 0, 4);
    uintptr_t d = (uintptr_t)detour;
    memcpy(t + 6, &d, 8);

    int32_t newRel = (int32_t)((intptr_t)t - (intptr_t)(thunk + 5));
    DWORD old;
    if (!VirtualProtect(thunk, 5, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    memcpy(thunk + 1, &newRel, 4);
    VirtualProtect(thunk, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), thunk, 5);
    Log("hooked %s (rva %06X)", b.name, (unsigned)b.fnRva);
    return original;
}

bool AllChecksPass(bool verbose) {
    struct { const ApiSig* s; const char* n; } apis[] = {
        {&kGetTop, "lua_gettop"}, {&kSetTop, "lua_settop"}, {&kToNumber, "lua_tonumber"},
        {&kPushNumber, "lua_pushnumber"}, {&kType, "lua_type"}, {&kToLString, "lua_tolstring"},
        {&kPushString, "lua_pushstring"}, {&kIsInOnlineEvent, "IsInOnlineEvent"}};
    bool ok = true;
    for (auto& a : apis)
        if (!CheckBytes(a.s->rva, a.s->bytes, 8)) {
            if (verbose) Log("check failed: %s at rva %06X", a.n, (unsigned)a.s->rva);
            ok = false;
        }
    const Binding* bs[] = {&kGetOpponentVehicleId, &kGetSelectedVehicleId, &kSetTimeOfDay, &kStopTraffic};
    for (const Binding* b : bs)
        if (!CheckBinding(*b)) {
            if (verbose) Log("check failed: %s registration", b->name);
            ok = false;
        }
    return ok;
}

DWORD WINAPI Worker(LPVOID) {
    LoadSettings();
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!SafeRead(g_base, &dos, sizeof(dos)) || !SafeRead(g_base + dos.e_lfanew, &nt, sizeof(nt))) {
        Log("cannot read the exe headers - plugin disabled");
        return 0;
    }
    if (nt.FileHeader.TimeDateStamp != kExpectedTimestamp) {
        Log("unsupported game build (PE timestamp %08X, expected %08X) - plugin disabled",
            (unsigned)nt.FileHeader.TimeDateStamp, kExpectedTimestamp);
        return 0;
    }
    // The protector decrypts the code after start-up: wait until everything checks out.
    bool ok = false;
    for (int i = 0; i < 1200 && !(ok = AllChecksPass(false)); ++i) Sleep(250);
    if (!ok) {
        AllChecksPass(true);
        Log("game code not as expected after 5 minutes - plugin disabled");
        return 0;
    }
    if (!AllocTrampolines()) {
        Log("could not allocate trampoline memory - plugin disabled");
        return 0;
    }
    lua_gettop = (Fn_gettop)(g_base + kGetTop.rva);
    lua_settop = (Fn_settop)(g_base + kSetTop.rva);
    lua_tonumber = (Fn_tonumber)(g_base + kToNumber.rva);
    lua_pushnumber = (Fn_pushnumber)(g_base + kPushNumber.rva);
    lua_type = (Fn_type)(g_base + kType.rva);
    lua_tolstring = (Fn_tolstring)(g_base + kToLString.rva);
    lua_pushstring = (Fn_pushstring)(g_base + kPushString.rva);

    // The original of GetSelectedVehicleId is called, not hooked.
    uint8_t* sel = (uint8_t*)(g_base + kGetSelectedVehicleId.fnRva);
    int32_t rel;
    memcpy(&rel, sel + 1, 4);
    g_origGetSelectedVehicleId = (lua_CFunction)(sel + 5 + rel);

    g_origStopTraffic = HookThunk(kStopTraffic, Hook_StopTraffic);
    g_origSetTimeOfDay = HookThunk(kSetTimeOfDay, Hook_SetTimeOfDay);
    g_origGetOpponentVehicleId = HookThunk(kGetOpponentVehicleId, Hook_GetOpponentVehicleId);
    Log("ready (Enabled=%d LogOnly=%d Time=%s Weather=%s)", S.enabled, S.logOnly, kTimeModeNames[S.timeMode],
        kWeatherModeNames[S.weatherMode]);

    if (IniInt(L"Overlay", L"Enabled", 1)) Overlay_Start();
    else Log("overlay: disabled in the ini");
    return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = h;
    DisableThreadLibraryCalls(h);
    InitializeCriticalSection(&g_settingsLock);
    InitializeCriticalSection(&g_logLock);
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    DefaultSettings(S);

    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(h, path, MAX_PATH);
    while (len && path[len - 1] != L'\\' && path[len - 1] != L'/') --len;
    path[len] = 0;
    lstrcpyW(g_iniPath, path);
    lstrcatW(g_iniPath, L"NFSHPR_EventRandomizer.ini");
    lstrcatW(path, L"NFSHPR_EventRandomizer.log");
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_rng = (uint64_t)q.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ 0x9E3779B97F4A7C15ULL;
    if (!g_rng) g_rng = 1;

    Log("NFSHPR Event Randomizer 1.0 - %d cars in the class table", kVehicleCount);
    HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}
