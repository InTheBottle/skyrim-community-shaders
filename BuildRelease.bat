@echo off

set "preset=ALL"
if NOT "%~1" == "" (
    set "preset=%~1"
)

echo Running preset %preset%

if exist "build\%preset%\CMakeCache.txt" (
    echo Build folder warm, skipping configure
) else (
    cmake -S . --preset=%preset%
    if %ERRORLEVEL% NEQ 0 exit 1
)
cmake --build --preset=%preset%
if %ERRORLEVEL% NEQ 0 exit 1
