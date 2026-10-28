@echo off
rem Build ZoomHUD (x86). Requires Visual Studio 2022 with C++ workload.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if errorlevel 1 (
  echo vcvars32.bat not found - fix the path in build.bat
  exit /b 1
)
cd /d "%~dp0"
cl /nologo /TP /LD /O2 /MT /W3 /EHsc zoomhud.c /link /DLL /OUT:zoomhud.dll user32.lib gdi32.lib iphlpapi.lib ws2_32.lib dnsapi.lib winmm.lib dbghelp.lib
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK: zoomhud.dll
del /q *.obj *.exp *.lib 2>nul
