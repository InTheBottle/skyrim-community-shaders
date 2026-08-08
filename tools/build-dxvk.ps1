<#
.SYNOPSIS
    Incrementally build the staged DXVK d3d11 and dxgi DLLs.

.PARAMETER BuildType
    Meson build type. Defaults to release.
#>
[CmdletBinding()]
param(
    [string]$BuildType = 'release'
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
$DxvkSrc   = Join-Path $RepoRoot 'extern\dxvk'
$BuildDir  = Join-Path $DxvkSrc 'build'
$D3d11Dll  = Join-Path $BuildDir 'src\d3d11\dxvk_d3d11.dll'
$DxgiDll   = Join-Path $BuildDir 'src\dxgi\dxvk_dxgi.dll'
$Stamp     = Join-Path $BuildDir '.cs-dxvk-sha'

# Initialize the submodule and required nested projects on a cold checkout.
$vulkanHdr = Join-Path $DxvkSrc 'include\vulkan\include\vulkan\vulkan.h'
if ((-not (Test-Path (Join-Path $DxvkSrc 'meson.build'))) -or (-not (Test-Path $vulkanHdr))) {
    Write-Host "[build-dxvk] initializing extern/dxvk submodule (+ nested Vulkan/SPIRV headers)..."
    # PowerShell 5.1 treats Git progress on stderr as a terminating error.
    $prevEAP = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & git -C $RepoRoot submodule update --init --recursive --force -- extern/dxvk 2>&1 | Write-Host
    $ErrorActionPreference = $prevEAP
}
if (-not (Test-Path (Join-Path $DxvkSrc 'meson.build'))) {
    Write-Warning "[build-dxvk] extern/dxvk still not checked out after submodule init. Skipping (mod ships without DXVK)."
    exit 0
}

$sha = ''
try { $sha = (& git -C $DxvkSrc rev-parse HEAD 2>$null) } catch {}
if (-not $sha) { $sha = 'unknown' }
$short = $sha.Substring(0, [Math]::Min(8, $sha.Length))

$haveDlls = (Test-Path $D3d11Dll) -and (Test-Path $DxgiDll)

if ($haveDlls -and (Test-Path $Stamp) -and ((Get-Content $Stamp -Raw).Trim() -eq $sha)) {
    Write-Host "[build-dxvk] DXVK d3d11+dxgi up to date ($short) - skipping"
    exit 0
}

$meson = (Get-Command meson -ErrorAction SilentlyContinue).Source
if (-not $meson) {
    if ($haveDlls) {
        Write-Warning "[build-dxvk] meson not found; reusing the existing DXVK build (it may be stale vs $short)."
        exit 0
    }
    Write-Warning "[build-dxvk] meson not found and no DXVK build present - the mod will ship WITHOUT the DXVK renderer. Install it with 'pip install meson ninja'."
    exit 0
}

Write-Host "[build-dxvk] building DXVK d3d11+dxgi ($short, $BuildType)..."

# Meson compile reconfigures existing builds and Ninja handles source changes incrementally.
if (-not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
    & $meson setup $BuildDir $DxvkSrc --vsenv --buildtype $BuildType `
        -Db_ndebug=true -Dcpp_args="/arch:AVX2" `
        -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false
    if ($LASTEXITCODE -ne 0) { Write-Error "[build-dxvk] meson setup failed"; exit 1 }
}

# Reapply release flags to existing build directories as well as new ones.
& $meson configure $BuildDir -Db_ndebug=true -Dcpp_args="/arch:AVX2"
if ($LASTEXITCODE -ne 0) { Write-Error "[build-dxvk] meson configure (ndebug + /arch:AVX2) failed"; exit 1 }

& $meson compile -C $BuildDir
if ($LASTEXITCODE -ne 0) { Write-Error "[build-dxvk] meson compile failed"; exit 1 }

if (-not ((Test-Path $D3d11Dll) -and (Test-Path $DxgiDll))) {
    Write-Error "[build-dxvk] build reported success but dxvk_d3d11.dll/dxvk_dxgi.dll are missing"
    exit 1
}

Set-Content -Path $Stamp -Value $sha -Encoding ascii
Write-Host "[build-dxvk] done ($short)"
exit 0
