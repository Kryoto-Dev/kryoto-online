@echo off
setlocal enabledelayedexpansion

rem ============================================================
rem  Build both DLLs, both architectures.
rem
rem    build.bat          Release -> relbuild\
rem    build.bat debug    Debug   -> debbuild\
rem
rem  The old version hunted for MSBuild at one hard-coded path -
rem  Visual Studio 2022 Professional - so it failed on Community,
rem  on Build Tools, and on anything newer, with "MSBuild not
rem  found" on machines that had a perfectly good one.
rem  vswhere is the supported way to ask, and it ships with every
rem  installer since 2017.
rem ============================================================

set "CONFIG=Release"
set "OUTDIR=relbuild"
if /i "%~1"=="debug" (
    set "CONFIG=Debug"
    set "OUTDIR=debbuild"
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * ^
        -requires Microsoft.Component.MSBuild ^
        -find MSBuild\**\Bin\MSBuild.exe 2^>nul`) do (
        if not defined MSBUILD set "MSBUILD=%%i"
    )
)

rem Already in a developer prompt? Then it is on PATH and vswhere is moot.
if not defined MSBUILD (
    for /f "delims=" %%i in ('where msbuild 2^>nul') do (
        if not defined MSBUILD set "MSBUILD=%%i"
    )
)

if not defined MSBUILD (
    echo [ERROR] MSBuild not found.
    echo.
    echo You need Visual Studio Build Tools 2022 or later.
    echo Install it here: https://visualstudio.microsoft.com/visual-cpp-build-tools/
    echo.
    echo Select "Desktop development with C++" during installation.
    echo.
    pause
    exit /b 1
)

echo Found MSBuild: %MSBUILD%
echo Configuration: %CONFIG%
echo.

rem kryotoO first: it is what the proxy loads, and building it first
rem means a broken core fails before the ten-times-longer proxy build.
call :build "kryotoO.vcxproj"       x64   "kryotoO.dll"       || exit /b 1
call :build "kryotoO.vcxproj"       Win32 "kryotoO32.dll"     || exit /b 1
call :build "kryoto_online.vcxproj" x64   "steam_api64.dll"   || exit /b 1
call :build "kryoto_online.vcxproj" Win32 "steam_api.dll"     || exit /b 1

rem Every plugin under plugins/, so a new plugin is picked up without
rem touching this file. Release|x64 only: the plugins link the release
rem libMinHook.x64.lib, which has no debug variant.
for /r "plugins" %%p in (*.vcxproj) do call :build "%%~dpnp" x64 "%%~np" || exit /b 1

echo ========================================
echo  All builds completed successfully
echo ========================================
echo  x64: %OUTDIR%\x64\steam_api64.dll + kryotoO.dll
echo  x86: %OUTDIR%\x86\steam_api.dll   + kryotoO32.dll
echo.
echo  Ship BOTH files from a folder together - the proxy loads
echo  the core from beside itself.
echo  The steam_overlay proxy builds as overlay_proxy.dll and is
echo  copied to version.dll (Unity) and winmm.dll (Unreal) at
echo  build time, so all three are ready to deploy.
echo ========================================
pause
exit /b 0

:build
setlocal
set "PROJ=%~1"
set "PLAT=%~2"
set "WHAT=%~3"
echo ========================================
echo  %WHAT%  [%CONFIG% ^| %PLAT%]
echo ========================================
"%MSBUILD%" "%~dp0%PROJ%" -p:Configuration=%CONFIG% -p:Platform=%PLAT% -m
if errorlevel 1 (
    echo.
    echo [ERROR] %WHAT% [%CONFIG% ^| %PLAT%] failed.
    pause
    endlocal & exit /b 1
)
echo.
endlocal & exit /b 0
