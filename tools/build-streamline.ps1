<#
.SYNOPSIS
    Incrementally build the Streamline fork plugins staged by Community Shaders.

.PARAMETER Config
    MSBuild configuration. Defaults to Develop.
#>
[CmdletBinding()]
param(
    [string]$Config = 'Develop'
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Split-Path -Parent $ScriptDir
$SlSrc     = Join-Path $RepoRoot 'extern\Streamline'
$Artifacts = Join-Path $SlSrc '_artifacts'
$ProjDir   = Join-Path $SlSrc '_project\vs2022'
$Sln       = Join-Path $ProjDir 'streamline.sln'

# Keep this list aligned with the top-level CMake staging filter.
$plugins = @('sl.interposer', 'sl.fsr', 'sl.fsr_g', 'sl.xess')

function Get-PluginDll([string]$name) { Join-Path $Artifacts "$name\${Config}_x64\$name.dll" }

# Initialize Streamline on a cold checkout.
if (-not (Test-Path (Join-Path $SlSrc 'premake.lua'))) {
    Write-Host "[build-streamline] initializing extern/Streamline submodule..."
    # PowerShell 5.1 treats Git progress on stderr as a terminating error.
    $prevEAP = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & git -C $RepoRoot submodule update --init --force -- extern/Streamline 2>&1 | Write-Host
    $ErrorActionPreference = $prevEAP
}
if (-not (Test-Path (Join-Path $SlSrc 'premake.lua'))) {
    Write-Warning "[build-streamline] extern/Streamline still not checked out after submodule init. Skipping (mod ships without the fork plugins)."
    exit 0
}

# Initialize the nested FFX and XeSS headers used by the fork plugins.
$ffxInc  = Join-Path $SlSrc 'external\fidelityfx-sdk\ffx-api\include'
$xessInc = Join-Path $SlSrc 'external\xess\inc'
if ((-not (Test-Path $ffxInc)) -or (-not (Test-Path $xessInc))) {
    Write-Host "[build-streamline] initializing Streamline nested submodules (FFX / XeSS headers)..."
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & git -C $SlSrc submodule update --init --force -- external/fidelityfx-sdk external/xess 2>&1 | Write-Host
    $ErrorActionPreference = $prev
    if (-not (Test-Path $ffxInc))  { Write-Warning "[build-streamline] FFX headers ($ffxInc) missing; sl.fsr may fail to compile." }
    if (-not (Test-Path $xessInc)) { Write-Warning "[build-streamline] XeSS headers ($xessInc) missing; sl.xess may fail to compile." }
}

# Dirty tracked sources bypass the commit-based incremental skip.
$sha = ''
try { $sha = (& git -C $SlSrc rev-parse HEAD 2>$null) } catch {}
if (-not $sha) { $sha = 'unknown' }
$short = $sha.Substring(0, [Math]::Min(8, $sha.Length))
$dirty = $false
try { $dirty = [bool](& git -C $SlSrc status --porcelain --untracked-files=no 2>$null) } catch { $dirty = $true }
$Stamp = Join-Path $Artifacts ".cs-sl-sha-$Config"

$dlls     = $plugins | ForEach-Object { Get-PluginDll $_ }
$haveDlls = ($dlls | ForEach-Object { Test-Path $_ }) -notcontains $false

if (-not $dirty -and $haveDlls -and (Test-Path $Stamp) -and ((Get-Content $Stamp -Raw).Trim() -eq $sha)) {
    Write-Host "[build-streamline] fork plugins up to date ($short) - skipping"
    exit 0
}

# Locate MSBuild before CMake initializes the compiler environment.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = $null
if (Test-Path $vswhere) {
    $msbuild = (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\amd64\MSBuild.exe' 2>$null | Select-Object -First 1)
    if (-not $msbuild) {
        $msbuild = (& $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1)
    }
}
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    if ($haveDlls) {
        Write-Warning "[build-streamline] MSBuild not found; reusing the existing fork plugins (they may be stale vs $short)."
        exit 0
    }
    Write-Warning "[build-streamline] MSBuild not found and no fork plugins present - the mod will ship WITHOUT the fork sl.* plugins (DLSS/FSR/XeSS unavailable). Install Visual Studio 2022+ with the C++ workload."
    exit 0
}

# Generate the project tree only when it is missing.
if (-not (Test-Path $Sln)) {
    Write-Host "[build-streamline] generating VS solution (setup.bat: packman + premake)..."
    # setup.bat requires the Streamline root as cmd's working directory.
    $prevEAP = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    & cmd /c "cd /d $SlSrc && .\setup.bat vs2022" 2>&1 | Write-Host
    $ErrorActionPreference = $prevEAP
    if (-not (Test-Path $Sln)) { Write-Error "[build-streamline] setup.bat did not produce $Sln"; exit 1 }
}

Write-Host "[build-streamline] building fork plugins [$($plugins -join ', ')] ($short, ${Config}_x64)..."

# Build projects directly because the generated solution rejects plugin target names.
foreach ($p in $plugins) {
    $vcxproj = Join-Path $ProjDir "$p.vcxproj"
    if (-not (Test-Path $vcxproj)) { Write-Error "[build-streamline] missing project $vcxproj"; exit 1 }
    & $msbuild $vcxproj /m /p:Configuration=$Config /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { Write-Error "[build-streamline] msbuild failed for $p"; exit 1 }
}

$missing = $plugins | Where-Object { -not (Test-Path (Get-PluginDll $_)) }
if ($missing) {
    Write-Error "[build-streamline] build reported success but missing: $($missing -join ', ')"
    exit 1
}

Set-Content -Path $Stamp -Value $sha -Encoding ascii
Write-Host "[build-streamline] done ($short)"
exit 0
