@echo off

echo,
echo you probably don't want to use this... it requires installing VC++ compiler...
echo much easier to just go into Windhawk's editor and compile this there.

pause

call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
cd /d "c:\save\repos\Personal\Windhawk taskbar icon magnify"
cl /LD /std:c++17 src/mod.wh.cpp /Fe:taskbar_icon_magnify.dll user32.lib gdi32.lib comctl32.lib ole32.lib oleaut32.lib shlwapi.lib version.lib
