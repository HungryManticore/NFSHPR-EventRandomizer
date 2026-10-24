# NFSHPR Event Randomizer

An ASI plugin for **Need for Speed: Hot Pursuit Remastered** (PC) that makes single-player events
less scripted:

- **Opponents and cops** are replaced by random cars from the class of the car you picked. A racer
  stays a racer and a cop stays a cop, so an Exotic Series car gets Exotic racers and Speed
  Enforcement cops. Cops that join mid-race are included.
- **Time of day**: as the event, random within a range, or a time you choose.
- **Weather**: as the event, random with adjustable chances, dry, wet roads, rain or storm.
- **In-game menu** (default **F5**), drawn over the game, to change all of this between events.

Online events are never changed.

| Racer series | Cop class |
|---|---|
| Sports | Traffic Police |
| Performance | Highway Patrol |
| Super | Rapid Deployment |
| Exotic | Speed Enforcement |
| Hyper | Special Response |

## Requirements

- Need for Speed: Hot Pursuit Remastered for PC, `NFS11Remastered.exe` with PE timestamp `613F368C`
  (459,255,296 bytes). The plugin may or may not work on other builds.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) installed as `dinput8.dll`.
- Windows 10 or 11 (the menu uses `d3dcompiler_47.dll`, which ships with them).
- To race any car in any event, a modified `GAMELOGIC/CHALLENGEINSTANCES.BNDL` that allows every car
  everywhere, such as Open Selection for All Events (Vanilla) by Xpr3xa (not included). Without it, the game's own car restrictions apply as usual.

## Installation

1. Download `NFSHPR_EventRandomizer.asi` from the [Releases](../../releases) page.
2. Put it in the game folder or its `scripts` subfolder, wherever your ASI loader looks for plugins.
3. Start the game. A note in the top-left corner says *"Event Randomizer ready - press F5 for options"*.

On the first start the plugin writes `NFSHPR_EventRandomizer.ini` and `NFSHPR_EventRandomizer.log`
next to itself.

## Using the menu

Press **F5** (or the key set in the ini) anywhere, for example on the event map. **F5** or **Esc**
closes the menu. You can use the mouse, or the arrow keys with Enter/Space. While the menu is open the
game ignores keyboard and mouse input.

Settings are saved to the ini as you change them and apply when the **next event loads**. If you open
the menu during a race, your changes affect the next event, not the current one.

## Settings (`NFSHPR_EventRandomizer.ini`)

The file is re-read at the start of every event, so you can also edit it while the game runs.
A commented copy is in [`config/NFSHPR_EventRandomizer.ini`](config/NFSHPR_EventRandomizer.ini).

| Section | Key | Meaning |
|---|---|---|
| General | `Enabled` | `0` turns the randomizer off |
| General | `LogOnly` | `1` = test mode: log what would change, change nothing |
| Opponents | `RandomizeRacers`, `RandomizeCops` | swap AI racers / cops |
| Opponents | `MatchPlayerClass` | `1` = use your car's class, `0` = keep each opponent's class |
| Opponents | `AvoidDuplicates` | no two opponents get the same car while the class has enough cars |
| Opponents | `AvoidPlayerModel` | opponents never get your car's model |
| Opponents | `Exclude` | vehicle ids never to pick, comma separated (ids are in the log) |
| TimeOfDay | `Mode` | `Default`, `Random` or `Fixed` |
| TimeOfDay | `FixedHour` | hour for `Fixed`, e.g. `19.5` = 19:30 |
| TimeOfDay | `KeepTimeFlow` | `1` = the clock runs as in the original event, `0` = it stands still |
| TimeOfDay | `MinHour`, `MaxHour` | start-hour range for `Random` |
| TimeOfDay | `ClearSunOverride` | drop the event's fixed sun direction when the time is changed |
| Weather | `Mode` | `Default`, `Random`, `Dry`, `Wet`, `Rain` or `Storm` |
| Weather | `Dry`, `Wet`, `Rain`, `Storm` | relative chances for `Random` |
| Overlay | `Enabled` | `0` = no in-game menu (the settings still apply) |
| Overlay | `Key` | menu key: `F1`-`F12`, or a virtual-key code number |
| Overlay | `FontSize` | menu text size in pixels, `0` = automatic |

## Known limitations

- Controller input is not blocked while the menu is open. Use the keyboard or mouse in the menu.
- Restarting an event loads it again, so it rolls new random cars and weather.
- Roadblocks and other scripted cop vehicles keep their original cars.
- Only the game build listed above is supported.

## How it works

The game's single-player events are Lua scripts (`GAMELOGIC/LUA.BNDL`, compiled Lua 5.1) that call
native functions registered by the game. Three of them matter here:

- `GetOpponentVehicleId(competitor)` returns the car for each AI racer or cop. It is called before
  every spawn, including cops that deploy mid-race.
- `SetTimeOfDay(start, end, cycle, isWet, isRain, isThunder, ...)` receives the event's time and
  weather.
- `StopTraffic` is the first step of every event script, so it marks the start of a new event.

In the protected executable, each of these natives is a 5-byte `jmp` thunk into obfuscated code. The
plugin redirects the thunk's jump to its own function, calls the original, and rewrites the value it
returns to Lua (or the arguments it receives) through a few of the game's own Lua 5.1 API functions.
The player's car comes from the game's `GetSelectedVehicleId`. The car classes are taken from
`VEHICLES/VEHICLELIST.BUNDLE` and built into [`src/VehicleTable.h`](src/VehicleTable.h).

The menu hooks `IDXGISwapChain::Present` through DXGI's vtable and draws with
[Dear ImGui](https://github.com/ocornut/imgui). It also subclasses the game window and hooks
DirectInput 8's `GetDeviceState`/`GetDeviceData`, so the game reads no input while the menu is open.

`tools/NFSHPR_Probe.cpp` is the read-only diagnostic plugin used to find the addresses. It logs where
the Lua bindings are registered and dumps the decrypted executable image for offline disassembly.

## Building

The release binary was built on Linux with clang + lld against the MinGW-w64 headers. It uses
`msvcrt.dll` as its C library and needs no MinGW runtime:

```sh
git clone --depth 1 https://github.com/mingw-w64/mingw-w64
MINGW_W64=$PWD/mingw-w64 ./build_clang.sh      # -> build/NFSHPR_EventRandomizer.asi
```

A Visual Studio build script is also provided, but it has not been tested. Run `build_msvc.bat` from
the repository root in an *x64 Native Tools Command Prompt*.

## Source layout

| Path | Contents |
|---|---|
| `src/NFSHPR_EventRandomizer.cpp` | game hooks, randomization, settings, start-up checks |
| `src/Overlay.cpp` | Direct3D 11 / DirectInput hooks and the ImGui menu |
| `src/Shared.h` | settings shared by both |
| `src/VehicleTable.h` | the 132 racer and cop cars with their class |
| `src/CrtShim.cpp` | `vsnprintf` and stack-probe helpers for the clang build |
| `third_party/imgui` | Dear ImGui 1.91.5 (MIT) |
| `tools/NFSHPR_Probe.cpp` | the diagnostic plugin |

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut and contributors (MIT licence).
- Car names come from the vehicle id list of the NFS-HPR-Archipelago project.
- Ultimate ASI Loader by ThirteenAG.

## Licence

MIT. See [LICENSE](LICENSE). Dear ImGui keeps its own licence in `third_party/imgui/LICENSE.txt`.
Not affiliated with Electronic Arts or Criterion Games.

## AI disclosure

The script was built using Claude.
