@echo off

set "preset=ALL"
if NOT "%~1" == "" (
    set "preset=%~1"
)

echo Running preset %preset%
if "%preset%" == "ALL" echo TIP: use 'BuildRelease.bat Dev-Fast' for fast warm iteration (Ninja, no LTO, no packaging)

rem Ninja-based presets need cl.exe on PATH; bootstrap the VS x64 dev
rem environment via vswhere when invoked from a plain shell.
if NOT "%preset%" == "Dev-Fast" goto :skipvsenv
where cl >nul 2>&1
if NOT ERRORLEVEL 1 goto :skipvsenv
echo Locating Visual Studio for the Ninja toolchain...
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo ERROR: No Visual Studio installation found; run from a VS x64 developer prompt instead
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: failed to initialize VS x64 toolchain environment
    exit /b 1
)
:skipvsenv

rem Parallelize across projects too (MSBuild /m); Ninja is parallel by default.
if not defined CMAKE_BUILD_PARALLEL_LEVEL set "CMAKE_BUILD_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%"

if exist "build\%preset%\CMakeCache.txt" (
    echo Build folder warm, skipping configure
) else (
    cmake -S . --preset=%preset%
    if %ERRORLEVEL% NEQ 0 exit /b 1
)
cmake --build --preset=%preset%
if %ERRORLEVEL% NEQ 0 exit /b 1
