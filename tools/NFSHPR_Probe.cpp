// NFSHPR_Probe.asi - diagnostic plugin for Need for Speed Hot Pursuit Remastered (x64)
//
// Read-only. It patches nothing and hooks nothing. It finds the native functions the game's
// Lua scripts call (GetOpponentVehicleId, GetChallengeParam, ...) and writes their addresses
// to NFSHPR_Probe.log next to this .asi. All addresses are logged as RVAs (offset from the
// exe's base), so they stay valid across launches.
//
// The scan runs automatically once the game window is up. Press F9 in game to scan again
// (for example in the main menu or during an event).
//
// Build (Visual Studio x64 Native Tools prompt):
//   cl /nologo /O2 /LD /GS- NFSHPR_Probe.cpp /link /OUT:NFSHPR_Probe.asi kernel32.lib user32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _MSC_VER
#include <stdio.h>
#define PROBE_VSNPRINTF _vsnprintf
#else
extern "C" int __cdecl _vsnprintf(char*, size_t, const char*, va_list);  // from ntdll
#define PROBE_VSNPRINTF _vsnprintf
#endif

#define X64 "%I64X"

namespace {

// ------------------------------------------------------------------ names to look for

enum Group { kFunc, kParam };
struct Name { const char* text; Group group; };

const Name kNames[] = {
    // Native functions called from Primitives.lua / Challenges.lua
    {"GetOpponentVehicleId", kFunc},  {"GetOpponentColourId", kFunc},
    {"GetOpponentToughness", kFunc},  {"GetSelectedVehicleId", kFunc},
    {"GetSelectedColourId", kFunc},   {"GetChallengeParam", kFunc},
    {"GetCompetitorType", kFunc},     {"IsCompetitorCop", kFunc},
    {"IsVehicleCop", kFunc},          {"SpawnVehicle", kFunc},
    {"IsVehicleLoaded", kFunc},       {"SetTimeOfDay", kFunc},
    {"SetTimeOfDayFromLobby", kFunc}, {"GetPlayerVehicleIndex", kFunc},
    {"ValidateAICarColour", kFunc},   {"SetupRaceCar", kFunc},
    // Event parameters read by the SetTimeOfDay primitive
    {"TimeOfDay", kParam},            {"FinishTimeOfDay", kParam},
    {"CycleTimeOfDay", kParam},       {"IsWet", kParam},
    {"IsRainActive", kParam},         {"IsThunderActive", kParam},
    {"IsAlternativeWeather", kParam}, {"OverrideSunDirection", kParam},
};
const int kNameCount = sizeof(kNames) / sizeof(kNames[0]);

// ------------------------------------------------------------------ tiny helpers (no CRT needed)

template <class T>
struct Vec {  // POD growable array on the process heap
    T* p;
    size_t n, cap;
    void push(const T& v) {
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 64;
            void* np = p ? HeapReAlloc(GetProcessHeap(), 0, p, nc * sizeof(T))
                         : HeapAlloc(GetProcessHeap(), 0, nc * sizeof(T));
            if (!np) return;
            p = (T*)np;
            cap = nc;
        }
        p[n++] = v;
    }
    void clear() { n = 0; }
};

HMODULE g_self;
HANDLE g_log = INVALID_HANDLE_VALUE;
uintptr_t g_base, g_end;
int g_scanNumber;

void Log(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[1536];
    buf[sizeof(buf) - 1] = 0;
    va_list ap;
    va_start(ap, fmt);
    int n = PROBE_VSNPRINTF(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    DWORD written;
    WriteFile(g_log, buf, (DWORD)n, &written, nullptr);
}

void Format(char* out, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = PROBE_VSNPRINTF(out, cap - 1, fmt, ap);
    va_end(ap);
    out[(n < 0 || (size_t)n >= cap) ? cap - 1 : (size_t)n] = 0;
}

bool SafeRead(uintptr_t addr, void* dst, size_t len) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void*)addr, dst, len, &got) && got == len;
}

uintptr_t Rva(uintptr_t a) { return a - g_base; }

// ------------------------------------------------------------------ image regions

struct Region { uintptr_t start, end; bool exec; };
Vec<Region> g_regions;

bool IsReadable(DWORD p) {
    p &= 0xFF;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}
bool IsExecProt(DWORD p) {
    p &= 0xFF;
    return p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

void CollectRegions() {
    g_regions.clear();
    uintptr_t a = g_base;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < g_end && VirtualQuery((void*)a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t s = (uintptr_t)mbi.BaseAddress, e = s + mbi.RegionSize;
        if (e <= a) break;
        if (e > g_end) e = g_end;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            IsReadable(mbi.Protect)) {
            bool x = IsExecProt(mbi.Protect);
            if (g_regions.n && g_regions.p[g_regions.n - 1].end == s &&
                g_regions.p[g_regions.n - 1].exec == x)
                g_regions.p[g_regions.n - 1].end = e;
            else
                g_regions.push(Region{s, e, x});
        }
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
}

const Region* FindRegion(uintptr_t a) {
    size_t lo = 0, hi = g_regions.n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const Region& r = g_regions.p[mid];
        if (a < r.start) hi = mid;
        else if (a >= r.end) lo = mid + 1;
        else return &r;
    }
    return nullptr;
}
bool IsCode(uintptr_t a) { const Region* r = FindRegion(a); return r && r->exec; }

// Calls f(buffer, bytesRead, scanLimit, addressOfBuffer) over a region in overlapping chunks.
// Hits must start below scanLimit so that the overlap is not reported twice.
const size_t kChunk = 16u << 20, kOverlap = 256;
uint8_t* g_chunk;

template <class F>
void ForEachChunk(const Region& r, F f) {
    if (!g_chunk) g_chunk = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, kChunk + kOverlap);
    if (!g_chunk) return;
    for (uintptr_t a = r.start; a < r.end; a += kChunk) {
        size_t want = (size_t)((r.end - a) < kChunk + kOverlap ? (r.end - a) : kChunk + kOverlap);
        size_t limit = want < kChunk ? want : kChunk;
        if (SafeRead(a, g_chunk, want)) {
            f(g_chunk, want, limit, a);
            continue;
        }
        for (size_t off = 0; off < limit; off += 0x1000) {  // fall back to single pages
            size_t len = limit - off < 0x1000 ? limit - off : 0x1000;
            if (SafeRead(a + off, g_chunk, len)) f(g_chunk, len, len, a + off);
        }
    }
}

// ------------------------------------------------------------------ scan results

struct StrHit { uintptr_t addr; int name; };
struct Ref { uintptr_t at; uintptr_t str; int name; int kind; };  // kind: 0 pointer, 1 lea, 2 mov imm64
struct Cand { int name; uintptr_t fn; int score; const char* how; long dist; };

Vec<StrHit> g_strings;  // sorted by address after the string pass
Vec<Ref> g_refs;
Vec<Cand> g_cands;

int FindString(uintptr_t a) {
    size_t lo = 0, hi = g_strings.n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_strings.p[mid].addr < a) lo = mid + 1;
        else hi = mid;
    }
    return (lo < g_strings.n && g_strings.p[lo].addr == a) ? (int)lo : -1;
}

void AddCand(int name, uintptr_t fn, int score, const char* how, long dist) {
    for (size_t i = 0; i < g_cands.n; ++i) {
        Cand& c = g_cands.p[i];
        if (c.name == name && c.fn == fn) {
            c.score += score;  // seen again: stronger
            return;
        }
    }
    g_cands.push(Cand{name, fn, score, how, dist});
}

void PassStrings() {
    g_strings.clear();
    bool first[256] = {};
    size_t len[kNameCount];
    for (int k = 0; k < kNameCount; ++k) {
        first[(uint8_t)kNames[k].text[0]] = true;
        len[k] = strlen(kNames[k].text);
    }
    int count[kNameCount] = {};
    for (size_t ri = 0; ri < g_regions.n; ++ri) {
        ForEachChunk(g_regions.p[ri], [&](const uint8_t* b, size_t got, size_t limit, uintptr_t base) {
            for (size_t i = 0; i < limit; ++i) {
                if (!first[b[i]]) continue;
                if (i > 0 && b[i - 1] != 0) continue;  // whole C string only
                for (int k = 0; k < kNameCount; ++k) {
                    if (kNames[k].text[0] != (char)b[i] || i + len[k] + 1 > got) continue;
                    if (b[i + len[k]] == 0 && memcmp(b + i, kNames[k].text, len[k]) == 0 && count[k] < 64) {
                        g_strings.push(StrHit{base + i, k});
                        ++count[k];
                    }
                }
            }
        });
    }
    for (size_t i = 1; i < g_strings.n; ++i) {  // insertion sort by address (small)
        StrHit v = g_strings.p[i];
        size_t j = i;
        while (j > 0 && g_strings.p[j - 1].addr > v.addr) { g_strings.p[j] = g_strings.p[j - 1]; --j; }
        g_strings.p[j] = v;
    }
}

void PassRefs() {
    g_refs.clear();
    if (!g_strings.n) return;
    uintptr_t lo = g_strings.p[0].addr, hi = g_strings.p[g_strings.n - 1].addr;
    for (size_t ri = 0; ri < g_regions.n; ++ri) {
        const Region& r = g_regions.p[ri];
        ForEachChunk(r, [&](const uint8_t* b, size_t got, size_t limit, uintptr_t base) {
            // 1) 8-byte pointers to a name (registration tables)
            size_t start = (8 - (base & 7)) & 7;
            for (size_t i = start; i + 8 <= got && i < limit; i += 8) {
                uint64_t v;
                memcpy(&v, b + i, 8);
                if (v < lo || v > hi) continue;
                int s = FindString((uintptr_t)v);
                if (s >= 0 && g_refs.n < 20000) g_refs.push(Ref{base + i, (uintptr_t)v, g_strings.p[s].name, 0});
            }
            if (!r.exec) return;
            // 2) code: lea r64,[rip+disp32] (48/4C 8D modrm=00 reg 101) and mov r64,imm64
            for (size_t i = 0; i + 10 <= got && i < limit; ++i) {
                uint8_t p0 = b[i];
                if ((p0 == 0x48 || p0 == 0x4C) && b[i + 1] == 0x8D && (b[i + 2] & 0xC7) == 0x05) {
                    int32_t d;
                    memcpy(&d, b + i + 3, 4);
                    uintptr_t t = base + i + 7 + (intptr_t)d;
                    if (t < lo || t > hi) continue;
                    int s = FindString(t);
                    if (s >= 0 && g_refs.n < 20000) g_refs.push(Ref{base + i, t, g_strings.p[s].name, 1});
                } else if ((p0 == 0x48 || p0 == 0x49) && b[i + 1] >= 0xB8 && b[i + 1] <= 0xBF) {
                    uint64_t v;
                    memcpy(&v, b + i + 2, 8);
                    if (v < lo || v > hi) continue;
                    int s = FindString((uintptr_t)v);
                    if (s >= 0 && g_refs.n < 20000) g_refs.push(Ref{base + i, (uintptr_t)v, g_strings.p[s].name, 2});
                }
            }
        });
    }
}

// ------------------------------------------------------------------ reporting

void Describe(uint64_t v, char* out, size_t cap) {
    out[0] = 0;
    if (v == 0) { lstrcpynA(out, "0", (int)cap); return; }
    const Region* r = FindRegion((uintptr_t)v);
    if (r && r->exec) { wsprintfA(out, "code rva %08X", (unsigned)Rva((uintptr_t)v)); return; }
    if (r) {
        char s[48];
        size_t n = 0;
        if (SafeRead((uintptr_t)v, s, sizeof(s))) {
            while (n < sizeof(s) && s[n] >= 0x20 && s[n] < 0x7F) ++n;
            if (n >= 3 && n < sizeof(s) && s[n] == 0) {
                s[n] = 0;
                char tmp[96];
                wsprintfA(tmp, "str \"%s\"", s);
                lstrcpynA(out, tmp, (int)cap);
                return;
            }
        }
        wsprintfA(out, "data rva %08X", (unsigned)Rva((uintptr_t)v));
        return;
    }
    Format(out, cap, "0x" X64, (unsigned long long)v);
}

void LogBytes(const char* label, uintptr_t a, size_t n) {
    uint8_t b[64];
    if (n > sizeof(b)) n = sizeof(b);
    if (!SafeRead(a, b, n)) { Log("%s <unreadable>\r\n", label); return; }
    char line[256];
    int o = wsprintfA(line, "%s", label);
    for (size_t i = 0; i < n; ++i) o += wsprintfA(line + o, " %02X", b[i]);
    Log("%s\r\n", line);
}

void ReportPointerRef(const Ref& r) {
    Log("    table entry at rva %08X:\r\n", (unsigned)Rva(r.at));
    for (int k = -3; k <= 4; ++k) {
        uint64_t v;
        uintptr_t at = r.at + (intptr_t)k * 8;
        if (!SafeRead(at, &v, 8)) continue;
        char d[128];
        Describe(v, d, sizeof(d));
        Log("      %+3d: %s\r\n", k * 8, d);
        if (k != 0 && IsCode((uintptr_t)v))
            AddCand(r.name, (uintptr_t)v, k == 1 ? 6 : (k == -1 || k == 2) ? 3 : 1, "table", k * 8);
    }
}

void ReportCodeRef(const Ref& r) {
    const Region* reg = FindRegion(r.at);
    if (!reg) return;
    uintptr_t ws = r.at > reg->start + 160 ? r.at - 160 : reg->start;
    uintptr_t we = r.at + 112 < reg->end ? r.at + 112 : reg->end;
    uint8_t w[288];
    size_t n = (size_t)(we - ws);
    if (n > sizeof(w) || !SafeRead(ws, w, n)) return;
    Log("    %s at rva %08X\r\n", r.kind == 1 ? "lea" : "mov imm64", (unsigned)Rva(r.at));
    int calls = 0;
    for (size_t j = 0; j + 7 <= n; ++j) {
        uintptr_t at = ws + j;
        long dist = (long)((intptr_t)at - (intptr_t)r.at);
        if ((w[j] == 0x48 || w[j] == 0x4C) && w[j + 1] == 0x8D && (w[j + 2] & 0xC7) == 0x05) {
            int32_t d;
            memcpy(&d, w + j + 3, 4);
            uintptr_t t = at + 7 + (intptr_t)d;
            if (t != r.str && IsCode(t)) {
                int reg64 = ((w[j + 2] >> 3) & 7) + (w[j] == 0x4C ? 8 : 0);
                Log("      lea r%d -> code rva %08X  (%+ld bytes)%s\r\n", reg64, (unsigned)Rva(t), dist,
                    (t & 15) ? "" : "  [16-aligned]");
                long ad = dist < 0 ? -dist : dist;
                AddCand(r.name, t, ad <= 40 ? 4 : 1, "lea near name", dist);
            }
        } else if (w[j] == 0xE8 && at > r.at && calls < 2 && j + 5 <= n) {
            int32_t d;
            memcpy(&d, w + j + 1, 4);
            uintptr_t t = at + 5 + (intptr_t)d;
            if (IsCode(t)) {
                Log("      call -> code rva %08X  (%+ld bytes)\r\n", (unsigned)Rva(t), dist);
                ++calls;
            }
        }
    }
}

void LogImageInfo() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    GetFileAttributesExW(path, GetFileExInfoStandard, &fa);
    char p8[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof(p8), nullptr, nullptr);
    Log("exe: %s\r\nexe size: %u bytes\r\n", p8, fa.nFileSizeLow);

    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!SafeRead(g_base, &dos, sizeof(dos)) || !SafeRead(g_base + dos.e_lfanew, &nt, sizeof(nt))) {
        Log("PE headers unreadable\r\n");
        return;
    }
    Log("PE timestamp: %08X  SizeOfImage: %08X  EntryPoint rva: %08X  sections: %u\r\n",
        (unsigned)nt.FileHeader.TimeDateStamp, (unsigned)nt.OptionalHeader.SizeOfImage,
        (unsigned)nt.OptionalHeader.AddressOfEntryPoint, (unsigned)nt.FileHeader.NumberOfSections);
    uintptr_t sh = g_base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                   nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections && i < 64; ++i) {
        IMAGE_SECTION_HEADER s;
        if (!SafeRead(sh + i * sizeof(s), &s, sizeof(s))) break;
        char nm[9] = {};
        memcpy(nm, s.Name, 8);
        Log("  section %-8s rva %08X vsize %08X flags %08X\r\n", nm, (unsigned)s.VirtualAddress,
            (unsigned)s.Misc.VirtualSize, (unsigned)s.Characteristics);
    }
}

void Scan() {
    ++g_scanNumber;
    DWORD t0 = GetTickCount();
    SYSTEMTIME st;
    GetLocalTime(&st);
    Log("\r\n==================== scan %d at %02u:%02u:%02u ====================\r\n", g_scanNumber,
        st.wHour, st.wMinute, st.wSecond);

    CollectRegions();
    size_t execBytes = 0, allBytes = 0;
    for (size_t i = 0; i < g_regions.n; ++i) {
        allBytes += g_regions.p[i].end - g_regions.p[i].start;
        if (g_regions.p[i].exec) execBytes += g_regions.p[i].end - g_regions.p[i].start;
    }
    Log("readable regions: %u (%u MB, of which code %u MB)\r\n", (unsigned)g_regions.n,
        (unsigned)(allBytes >> 20), (unsigned)(execBytes >> 20));

    PassStrings();
    PassRefs();
    g_cands.clear();

    for (int k = 0; k < kNameCount; ++k) {
        Log("\r\n--- %s (%s)\r\n", kNames[k].text, kNames[k].group == kFunc ? "function" : "parameter");
        int ns = 0;
        for (size_t i = 0; i < g_strings.n; ++i)
            if (g_strings.p[i].name == k) {
                Log("  string at rva %08X\r\n", (unsigned)Rva(g_strings.p[i].addr));
                ++ns;
            }
        if (!ns) { Log("  string not found\r\n"); continue; }
        int shown = 0, total = 0;
        for (size_t i = 0; i < g_refs.n; ++i) {
            const Ref& r = g_refs.p[i];
            if (r.name != k) continue;
            ++total;
            if (shown >= 12) continue;
            ++shown;
            if (r.kind == 0) ReportPointerRef(r);
            else ReportCodeRef(r);
        }
        if (!total) Log("  no references found\r\n");
        else if (total > shown) Log("  (%d more references not shown)\r\n", total - shown);
    }

    Log("\r\n==================== summary (best guesses) ====================\r\n");
    for (int k = 0; k < kNameCount; ++k) {
        if (kNames[k].group != kFunc) continue;
        Log("%-22s", kNames[k].text);
        bool any = false;
        for (int rank = 0; rank < 3; ++rank) {  // top 3 by score
            int best = -1;
            for (size_t i = 0; i < g_cands.n; ++i) {
                Cand& c = g_cands.p[i];
                if (c.name != k || c.score <= 0) continue;
                if (best < 0 || c.score > g_cands.p[best].score) best = (int)i;
            }
            if (best < 0) break;
            Cand& c = g_cands.p[best];
            Log("  rva %08X (score %d, %s %+ld)", (unsigned)Rva(c.fn), c.score, c.how, c.dist);
            c.score = -c.score;  // mark as printed
            any = true;
        }
        Log(any ? "\r\n" : "  none\r\n");
    }
    for (size_t i = 0; i < g_cands.n; ++i) {
        Cand& c = g_cands.p[i];
        if (c.score >= 0 || -c.score < 4) continue;  // prologues of the stronger candidates
        char label[96];
        wsprintfA(label, "  bytes %s @ rva %08X:", kNames[c.name].text, (unsigned)Rva(c.fn));
        LogBytes(label, c.fn, 32);
    }
    Log("\r\nscan %d took %u ms\r\n", g_scanNumber, (unsigned)(GetTickCount() - t0));
}

// ------------------------------------------------------------------ dump of the decrypted image
// Writes the exe's memory from its base up to the protector's ".link" section (code, read-only
// data, globals, .pdata) to NFSHPR_dump.bin, so the code can be disassembled offline. Pages that
// cannot be read are written as zeros. Byte N of the file is RVA N.

void DumpImage(const wchar_t* dir) {
    uintptr_t stop = 0;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (SafeRead(g_base, &dos, sizeof(dos)) && SafeRead(g_base + dos.e_lfanew, &nt, sizeof(nt))) {
        uintptr_t sh = g_base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                       nt.FileHeader.SizeOfOptionalHeader;
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections && i < 64; ++i) {
            IMAGE_SECTION_HEADER s;
            if (!SafeRead(sh + i * sizeof(s), &s, sizeof(s))) break;
            if (memcmp(s.Name, ".link", 6) == 0) stop = s.VirtualAddress;
        }
    }
    if (!stop || stop > 0x4000000) stop = 0x1803000;  // fallback: the layout seen in scan 1

    wchar_t path[MAX_PATH];
    lstrcpyW(path, dir);
    lstrcatW(path, L"NFSHPR_dump.bin");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("dump: cannot create NFSHPR_dump.bin\r\n"); return; }
    if (!g_chunk) g_chunk = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, kChunk + kOverlap);
    if (!g_chunk) { CloseHandle(f); return; }

    unsigned bad = 0;
    bool ok = true;
    for (uintptr_t off = 0; off < stop && ok; off += 0x100000) {
        size_t len = (size_t)(stop - off < 0x100000 ? stop - off : 0x100000);
        if (!SafeRead(g_base + off, g_chunk, len)) {
            for (size_t p = 0; p < len; p += 0x1000)
                if (!SafeRead(g_base + off + p, g_chunk + p, 0x1000)) {
                    memset(g_chunk + p, 0, 0x1000);
                    ++bad;
                }
        }
        DWORD w = 0;
        ok = WriteFile(f, g_chunk, (DWORD)len, &w, nullptr) && w == len;
    }
    CloseHandle(f);
    Log("\r\ndump: NFSHPR_dump.bin, rva 0 to %08X (%u MB), %u unreadable pages zeroed%s\r\n",
        (unsigned)stop, (unsigned)(stop >> 20), bad, ok ? "" : " - WRITE FAILED");
}

// ------------------------------------------------------------------ startup

BOOL CALLBACK FindOwnWindow(HWND h, LPARAM found) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
        *(bool*)found = true;
        return FALSE;
    }
    return TRUE;
}

DWORD WINAPI Worker(LPVOID) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(g_self, path, MAX_PATH);
    while (len && path[len - 1] != L'\\' && path[len - 1] != L'/') --len;
    path[len] = 0;
    wchar_t dir[MAX_PATH];
    lstrcpyW(dir, path);
    lstrcpyW(path + len, L"NFSHPR_Probe.log");
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);

    HMODULE exe = GetModuleHandleW(nullptr);
    g_base = (uintptr_t)exe;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    g_end = g_base + 0x1000;
    if (SafeRead(g_base, &dos, sizeof(dos)) && SafeRead(g_base + dos.e_lfanew, &nt, sizeof(nt)))
        g_end = g_base + nt.OptionalHeader.SizeOfImage;

    Log("NFSHPR_Probe v2 - read-only diagnostic. F9 = scan again.\r\n");
    LogImageInfo();

    bool found = false;
    for (int s = 0; s < 180 && !found; ++s) {  // wait for the game window (max 3 min)
        EnumWindows(FindOwnWindow, (LPARAM)&found);
        if (!found) Sleep(1000);
    }
    Log(found ? "game window is up\r\n" : "no game window after 3 minutes, scanning anyway\r\n");
    Sleep(5000);
    Scan();
    DumpImage(dir);

    bool wasDown = false;
    for (;;) {
        bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !wasDown) Scan();
        wasDown = down;
        Sleep(100);
    }
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
