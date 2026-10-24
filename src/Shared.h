// Shared between the randomizer core (NFSHPR_EventRandomizer.cpp) and the overlay (Overlay.cpp).
#pragma once
#include <windows.h>
#include <stdint.h>

enum TimeMode { kTimeDefault = 0, kTimeRandom = 1, kTimeFixed = 2 };
enum WeatherMode { kWeatherDefault = 0, kWeatherRandom = 1, kWeatherDry = 2, kWeatherWet = 3, kWeatherRain = 4, kWeatherStorm = 5 };

struct Settings {
    // [General]
    int enabled, logOnly;
    // [Opponents]
    int racers, cops, matchClass, avoidDuplicates, avoidPlayerModel;
    uint32_t exclude[64];
    int excludeCount;
    // [TimeOfDay]
    int timeMode;          // TimeMode
    float fixedHour;       // 0..24, used by kTimeFixed
    int keepTimeFlow;      // 1 = keep the event's start->end time span, 0 = clock stands still
    int minHour, maxHour;  // range for kTimeRandom
    int clearSunOverride;
    // [Weather]
    int weatherMode;       // WeatherMode
    int wDry, wWet, wRain, wStorm;
    // [Overlay]
    int menuKey;           // virtual-key code
    int fontSize;          // 0 = automatic
};

extern Settings S;                 // guarded by g_settingsLock for writers
extern CRITICAL_SECTION g_settingsLock;
extern uintptr_t g_base;

void Log(const char* fmt, ...);
void LoadSettings();   // ini -> S
void SaveSettings();   // S -> ini
void DefaultSettings(Settings& s);
const char* KeyName(int vk, char* out);  // "F5", ...

void Overlay_Start();  // called once the game code is verified and the game hooks are in
