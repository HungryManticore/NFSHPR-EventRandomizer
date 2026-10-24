@echo off
rem Builds NFSHPR_EventRandomizer.asi with Visual Studio.
rem Run from the repository root in "x64 Native Tools Command Prompt for VS".
set DEFS=/DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS /DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS /DIMGUI_DISABLE_FILE_FUNCTIONS /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /DIMGUI_DISABLE_DEBUG_TOOLS
set IMGUI=third_party\imgui
if not exist build mkdir build
cl /nologo /O2 /LD /GS- /std:c++17 %DEFS% /I src /I %IMGUI% /I %IMGUI%\backends /Fobuild\ src\NFSHPR_EventRandomizer.cpp src\Overlay.cpp src\CrtShim.cpp %IMGUI%\imgui.cpp %IMGUI%\imgui_draw.cpp %IMGUI%\imgui_tables.cpp %IMGUI%\imgui_widgets.cpp %IMGUI%\backends\imgui_impl_dx11.cpp %IMGUI%\backends\imgui_impl_win32.cpp /link /OUT:build\NFSHPR_EventRandomizer.asi kernel32.lib user32.lib gdi32.lib d3dcompiler.lib dwmapi.lib
