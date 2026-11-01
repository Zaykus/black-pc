@echo off
setlocal
echo Building BLACK PC Mod Menu (d3d9.dll)...

set COMPILER=..\tools\w64devkit\bin\g++.exe
set IMGUI=..\tools\imgui
set OUT=..\port\d3d9.dll

if not exist %COMPILER% (
    set COMPILER=g++.exe
)

%COMPILER% -O2 -c d3d9_hook.cpp -I%IMGUI% -o d3d9_hook.o
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

%COMPILER% -O2 -c %IMGUI%\imgui.cpp -I%IMGUI% -o imgui.o
%COMPILER% -O2 -c %IMGUI%\imgui_draw.cpp -I%IMGUI% -o imgui_draw.o
%COMPILER% -O2 -c %IMGUI%\imgui_tables.cpp -I%IMGUI% -o imgui_tables.o
%COMPILER% -O2 -c %IMGUI%\imgui_widgets.cpp -I%IMGUI% -o imgui_widgets.o
%COMPILER% -O2 -c %IMGUI%\backends\imgui_impl_dx9.cpp -I%IMGUI% -o imgui_impl_dx9.o
%COMPILER% -O2 -c %IMGUI%\backends\imgui_impl_win32.cpp -I%IMGUI% -o imgui_impl_win32.o

%COMPILER% -shared -o %OUT% d3d9_hook.o imgui.o imgui_draw.o imgui_tables.o imgui_widgets.o imgui_impl_dx9.o imgui_impl_win32.o d3d9_hook.def -static -ld3d9 -ldwmapi -lgdi32 -luser32
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%

echo Build successful! Output: %OUT%
