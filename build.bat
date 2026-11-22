@echo off
:: ============================================================================
:: Standalone Build Script for `rng_steam.dll`
:: Usage:
::   build.bat             -> builds build\rng_steam.dll
::   build.bat --deploy    -> builds and copies to MHW nativePC\plugins\mhw_rng\
:: ============================================================================
setlocal

set "ROOT=%~dp0"
set "INCDIR=%ROOT%include"
set "OUTDIR=%ROOT%build"
set "GAME_PLUG_DIR=C:\Program Files (x86)\Steam\steamapps\common\Monster Hunter World\nativePC\plugins\mhw_rng"

if not exist "%INCDIR%\mhw_rng_api.h" (
    if exist "%ROOT%sync_sdk.bat" call "%ROOT%sync_sdk.bat"
)
if not exist "%INCDIR%\mhw_rng_api.h" (
    echo ERROR: Missing "%INCDIR%\mhw_rng_api.h". Run sync_sdk.bat ^<path-to-mhw-rng^> first.
    exit /b 1
)

where cl.exe >nul 2>&1
if not errorlevel 1 goto :have_cl

set "VSDIR="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
)
if "%VSDIR%"=="" if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
)
if "%VSDIR%"=="" (
    echo ERROR: MSVC x64 toolchain not found.
    exit /b 1
)
set "VSCMD_SKIP_SENDTELEMETRY=1"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:have_cl
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo ===== Building rng_steam.dll =====
cl.exe /nologo /std:c++17 /O2 /GL /GR- ^
    /W4 /WX- /EHsc /MT ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "NOMINMAX" ^
    /utf-8 ^
    /I "%INCDIR%" ^
    "%ROOT%steam_plugin.cpp" ^
    /Fe:"%OUTDIR%\rng_steam.dll" ^
    /Fo:"%OUTDIR%\\" ^
    /LD ^
    /link kernel32.lib /OPT:REF /OPT:ICF ^
    /OUT:"%OUTDIR%\rng_steam.dll"
if errorlevel 1 exit /b 1

echo [OK] Built "%OUTDIR%\rng_steam.dll"

if /I "%~1"=="--deploy" (
    if not exist "%GAME_PLUG_DIR%" mkdir "%GAME_PLUG_DIR%"
    copy /y "%OUTDIR%\rng_steam.dll" "%GAME_PLUG_DIR%\rng_steam.dll" >nul
    if errorlevel 1 (
        echo ERROR: Failed copying to "%GAME_PLUG_DIR%\rng_steam.dll"
        exit /b 1
    )
    echo [OK] Deployed to "%GAME_PLUG_DIR%\rng_steam.dll" ^(type `$rng reload steam` in game^)
)

exit /b 0
