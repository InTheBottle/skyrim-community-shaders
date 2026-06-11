@echo off

rem Usage: BuildRelease.bat [BUILD_PRESET] [CONFIGURE_PRESET]
rem   BUILD_PRESET      cmake --build preset (default: ALL)
rem   CONFIGURE_PRESET  cmake configure preset (default: same as BUILD_PRESET)
rem Configure runs automatically when build\<CONFIGURE_PRESET>\CMakeCache.txt
rem is missing, so no preset ever needs a manual cmake configure step.
rem One-click wrappers: BuildDev.bat, BuildDevFast.bat, BuildPR.bat,
rem BuildDebug.bat.

set "preset=ALL"
if NOT "%~1" == "" (
    set "preset=%~1"
)
set "configpreset=%preset%"
if NOT "%~2" == "" (
    set "configpreset=%~2"
)

echo Running build preset %preset% (configure preset %configpreset%)
if "%preset%" == "ALL" echo TIP: use 'BuildDevFast.bat' for fast warm iteration (Ninja, no LTO, no packaging)

rem Ninja-based presets need cl.exe on PATH; bootstrap the VS x64 dev
rem environment via vswhere when invoked from a plain shell.
if NOT "%configpreset%" == "Dev-Fast" goto :skipvsenv
where cl >nul 2>&1
if NOT ERRORLEVEL 1 goto :skipvsenv
echo Locating Visual Studio for the Ninja toolchain...
rem Invoke vswhere directly and read its output from a temp file. A for /f
rem backquote command cannot hold this path: cmd /c strips the quotes (path
rem has special chars) and the (x86) parens break the for-parens parse when
rem unquoted. Clear VSINSTALL first so an inherited value can't mask a
rem lookup failure.
set "VSINSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found; run from a VS x64 developer prompt instead
    exit /b 1
)
"%VSWHERE%" -latest -products * -property installationPath > "%TEMP%\cs_vsinstall.txt"
set /p VSINSTALL=<"%TEMP%\cs_vsinstall.txt"
del "%TEMP%\cs_vsinstall.txt" >nul 2>&1
if not defined VSINSTALL (
    echo ERROR: No Visual Studio installation found; run from a VS x64 developer prompt instead
    exit /b 1
)
rem 2>&1: vcvars64 internals probe for vswhere on PATH and print a harmless
rem "not recognized" complaint on stderr; our ERRORLEVEL check below is the
rem real failure signal.
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: failed to initialize VS x64 toolchain environment
    exit /b 1
)
:skipvsenv

rem Parallelize across projects too (MSBuild /m); Ninja is parallel by default.
if not defined CMAKE_BUILD_PARALLEL_LEVEL set "CMAKE_BUILD_PARALLEL_LEVEL=%NUMBER_OF_PROCESSORS%"

rem Note: failure checks use 'if errorlevel 1' (dynamic) rather than
rem %ERRORLEVEL% inside a parenthesized block, where it would expand at
rem parse time and never observe the command's actual exit code.
if exist "build\%configpreset%\CMakeCache.txt" (
    echo Build folder warm, skipping configure
    goto :build
)
cmake -S . --preset=%configpreset%
if errorlevel 1 exit /b 1

:build
cmake --build --preset=%preset%
if errorlevel 1 exit /b 1
