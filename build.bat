@echo off
setlocal EnableDelayedExpansion

echo === Folder Size Viewer - Build Script ===
echo.

REM ---- Try MinGW g++ first ----
set GPP=
for %%P in (g++.exe) do set GPP=%%~$PATH:P

if "!GPP!"=="" (
    REM WinGet WinLibs location
    set "WL=%LOCALAPPDATA%\Microsoft\WinGet\Packages"
    for /R "!WL!" %%F in (g++.exe) do (
        if "!GPP!"=="" set "GPP=%%F"
    )
)

if not "!GPP!"=="" (
    echo Using MinGW g++: !GPP!
    "!GPP!" -std=c++17 -O2 -mwindows ^
        FolderSizeViewer.cpp -o FolderSizeViewer.exe ^
        -lcomctl32 -lshlwapi -lshell32 -lcomdlg32 -lole32
    goto :done
)

REM ---- Try MSVC ----
set VCVARS=
for %%Y in (2022 2019 2017) do (
    for %%E in (Community Professional Enterprise BuildTools) do (
        set "T=C:\Program Files\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat"
        if exist "!T!" ( set "VCVARS=!T!" & goto :vs_found )
        set "T=C:\Program Files (x86)\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat"
        if exist "!T!" ( set "VCVARS=!T!" & goto :vs_found )
    )
)

:vs_found
if not "!VCVARS!"=="" (
    echo Using MSVC: !VCVARS!
    call "!VCVARS!" > nul 2>&1
    cl /nologo /EHsc /W3 /O2 /std:c++17 /D UNICODE /D _UNICODE ^
       FolderSizeViewer.cpp ^
       /link /SUBSYSTEM:WINDOWS ^
       comctl32.lib shlwapi.lib shell32.lib user32.lib gdi32.lib comdlg32.lib ole32.lib ^
       /out:FolderSizeViewer.exe
    del /Q *.obj 2>nul
    goto :done
)

echo [ERROR] No C++ compiler found (MinGW g++ or MSVC).
echo Install one of:
echo   - MinGW via WinGet:  winget install BrechtSanders.WinLibs.POSIX.UCRT
echo   - Visual Studio:     https://visualstudio.microsoft.com/downloads/
pause
exit /b 1

:done
if exist FolderSizeViewer.exe (
    echo.
    echo [SUCCESS] FolderSizeViewer.exe ready!
    echo Run: FolderSizeViewer.exe
) else (
    echo.
    echo [FAILED] Build failed.
)
echo.
pause
