@echo off

set preset="ALL"
set skip_configure=0

:parse_args
if "%1"=="" goto done_args
if /I "%1"=="--build-only" (
    set skip_configure=1
    shift
    goto parse_args
)
if not "%1"=="" (
    set preset=%1
    shift
    goto parse_args
)
:done_args

echo Running preset %preset%

if "%skip_configure%"=="1" (
    echo Skipping CMake configure ^(--build-only^)
) else (
    cmake -S . --preset=%preset%
    if %ERRORLEVEL% NEQ 0 exit 1
)

cmake --build --preset=%preset%
if %ERRORLEVEL% NEQ 0 exit 1
