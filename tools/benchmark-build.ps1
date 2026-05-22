#Requires -Version 5.1
<#
.SYNOPSIS
    Benchmarks each major build phase and reports timings.

.DESCRIPTION
    Runs cmake configure, DLL compilation, AIO packaging, and local shader
    validation in sequence, timing each phase. Use this to measure the impact
    of build-system changes.

.PARAMETER Preset
    CMake preset to benchmark. Defaults to ALL. Use FastDev for the
    faster RelWithDebInfo variant (no LTO, ~3-4x quicker link step).

.PARAMETER SkipConfigure
    Skip the cmake configure phase (assumes build directory already exists).

.PARAMETER SkipValidation
    Skip the local hlslkit shader validation phase.

.PARAMETER Jobs
    Number of parallel jobs for hlslkit-compile. Defaults to the number of
    logical CPU cores.

.EXAMPLE
    # Full benchmark with default (ALL/Release) preset
    powershell.exe -File tools/benchmark-build.ps1

.EXAMPLE
    # Benchmark the fast developer preset (no LTO)
    powershell.exe -File tools/benchmark-build.ps1 -Preset FastDev

.EXAMPLE
    # Measure only the compile + package phases (skip configure and validation)
    powershell.exe -File tools/benchmark-build.ps1 -SkipConfigure -SkipValidation
#>

param(
    [string]$Preset = "ALL",
    [switch]$SkipConfigure,
    [switch]$SkipValidation,
    [int]$Jobs = [Environment]::ProcessorCount
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-Timed {
    param([string]$Label, [scriptblock]$Block)
    Write-Host ""
    Write-Host ">>> $Label" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        & $Block
        if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) {
            throw "Command exited with code $LASTEXITCODE"
        }
    } finally {
        $sw.Stop()
    }
    $elapsed = $sw.Elapsed
    $fmt = "{0:D2}m {1:D2}s" -f [int]$elapsed.TotalMinutes, $elapsed.Seconds
    Write-Host "<<< $Label completed in $fmt" -ForegroundColor Green
    return $elapsed
}

$results = [ordered]@{}
$buildDir = "build/$Preset"

Write-Host "============================================" -ForegroundColor Yellow
Write-Host "  Community Shaders build benchmark"       -ForegroundColor Yellow
Write-Host "  Preset   : $Preset"                       -ForegroundColor Yellow
Write-Host "  Build dir: $buildDir"                     -ForegroundColor Yellow
Write-Host "  CPU cores: $Jobs"                         -ForegroundColor Yellow
Write-Host "============================================" -ForegroundColor Yellow

# ── Configure ─────────────────────────────────────────────────────────────────
if (-not $SkipConfigure) {
    $results["Configure"] = Invoke-Timed "CMake configure (--preset $Preset)" {
        cmake -S . --preset $Preset
    }
} else {
    Write-Host "Skipping configure (--SkipConfigure)" -ForegroundColor DarkGray
}

# ── DLL compile ───────────────────────────────────────────────────────────────
$results["Compile DLL"] = Invoke-Timed "Compile CommunityShaders.dll" {
    cmake --build $buildDir --target CommunityShaders --config Release
}

# ── AIO packaging ─────────────────────────────────────────────────────────────
$results["Package AIO"] = Invoke-Timed "Create AIO zip (Package-AIO-Manual)" {
    cmake --build $buildDir --target Package-AIO-Manual
}

# ── Shader validation ─────────────────────────────────────────────────────────
if (-not $SkipValidation) {
    # Prepare shaders (copies HLSL files into AIO layout)
    $results["Prepare Shaders"] = Invoke-Timed "Prepare shaders for validation" {
        cmake --build $buildDir --target prepare_shaders
    }

    # Locate fxc.exe
    $fxcPath = ""
    $fxcCmd = Get-Command fxc.exe -ErrorAction SilentlyContinue
    if ($fxcCmd) {
        $fxcPath = $fxcCmd.Source
    } else {
        $sdkRoot = "C:\Program Files (x86)\Windows Kits\10\bin"
        if (Test-Path $sdkRoot) {
            Get-ChildItem $sdkRoot -Directory | Sort-Object Name -Descending | ForEach-Object {
                $candidate = Join-Path $_.FullName "x64\fxc.exe"
                if ((Test-Path $candidate) -and -not $fxcPath) { $fxcPath = $candidate }
            }
        }
    }

    if ($fxcPath) {
        $results["Shader Validation (Flatrim)"] = Invoke-Timed "hlslkit-compile Flatrim ($Jobs jobs)" {
            hlslkit-compile `
                --fxc $fxcPath `
                --shader-dir "$buildDir/aio/Shaders" `
                --output-dir build/ShaderCache `
                --config .github/configs/shader-validation.yaml `
                --max-warnings 0 `
                --suppress-warnings X1519 `
                --jobs $Jobs `
                --strip-debug-defines
        }
        $results["Shader Validation (VR)"] = Invoke-Timed "hlslkit-compile VR ($Jobs jobs)" {
            hlslkit-compile `
                --fxc $fxcPath `
                --shader-dir "$buildDir/aio/Shaders" `
                --output-dir build/ShaderCache `
                --config .github/configs/shader-validation-vr.yaml `
                --max-warnings 0 `
                --suppress-warnings X1519 `
                --jobs $Jobs `
                --strip-debug-defines
        }
    } else {
        Write-Warning "fxc.exe not found — skipping shader validation timing."
        Write-Warning "Install Windows SDK or add fxc.exe to PATH."
    }
}

# ── Summary ───────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "============================================" -ForegroundColor Yellow
Write-Host "  Benchmark results" -ForegroundColor Yellow
Write-Host "============================================" -ForegroundColor Yellow

$totalSeconds = 0
foreach ($phase in $results.Keys) {
    $elapsed = $results[$phase]
    $totalSeconds += $elapsed.TotalSeconds
    $fmt = "{0:D2}m {1:D2}s" -f [int]$elapsed.TotalMinutes, $elapsed.Seconds
    Write-Host ("  {0,-35} {1}" -f $phase, $fmt)
}

Write-Host "--------------------------------------------"
$totalFmt = "{0:D2}m {1:D2}s" -f [int]($totalSeconds / 60), [int]($totalSeconds % 60)
Write-Host ("  {0,-35} {1}" -f "TOTAL", $totalFmt) -ForegroundColor Cyan
Write-Host ""
Write-Host "Tip: run with -Preset FastDev to benchmark the no-LTO build." -ForegroundColor DarkGray
Write-Host "Tip: run with -SkipConfigure to skip configure on repeat runs." -ForegroundColor DarkGray
