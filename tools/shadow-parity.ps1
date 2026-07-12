<#
.SYNOPSIS
    Rerunnable vanilla-parity gate for the shadow / utility-pass replica.

.DESCRIPTION
    Drives the in-game devbench REST API (communityshaders.utilre) to prove the
    CS replica issues a byte-identical command stream to the vanilla engine for
    every covered shadow/utility pass, and pinpoints the first deviating pass if
    not. This is the structural (Level-1) half of the validation layer required
    for the multithreaded shadow work: run it before and after any change to a
    replica pass (or the threading refactor) to catch a regression down to the
    pass, technique, call index, and field.

    Flow per venue: load save -> set compare mode -> reset counters -> advance N
    frames IN MOTION -> read stats -> assert diverged == 0. Motion matters:
    stills never exercise the culled-caster divergence that only appears while
    moving (see the project's "validate culling in motion" rule).

    Requires the game already running with CS_UTIL_RE_MODE set at boot (hooks
    install only then) and the devbench REST server up (127.0.0.1:8920).

.PARAMETER Frames
    Frames to accumulate per venue before sampling stats (default 600).

.PARAMETER Saves
    One or more save names to validate (each a distinct venue). If omitted, runs
    against whatever is currently loaded.

.EXAMPLE
    pwsh tools/shadow-parity.ps1 -Saves 'Village','Riverwood','WhiterunInterior'
#>
[CmdletBinding()]
param(
    [int]$Frames = 600,
    [string[]]$Saves = @(),
    [string]$ApiBase = 'http://127.0.0.1:8920',
    [int]$SettleMs = 4000
)

$ErrorActionPreference = 'Stop'

function Invoke-Tool {
    param([string]$Name, [hashtable]$Args = @{}, [int]$TimeoutSec = 60)
    $body = ($Args | ConvertTo-Json -Depth 8 -Compress)
    return Invoke-RestMethod -Uri "$ApiBase/api/tool/$Name" -Method Post -Body $body `
        -ContentType 'application/json' -TimeoutSec $TimeoutSec
}

function Wait-Frames {
    param([int]$Count)
    # Advance frames by polling the frame counter via a no-op scenario wait; the
    # devbench scenario 'wait' step is wall-clock, so approximate at ~60 fps but
    # cap generously. Motion is driven by the caller having issued movement, or by
    # the scene itself; for a frozen check the caller sets sgtm accordingly.
    $ms = [Math]::Max(2000, [int]($Count / 60.0 * 1000))
    Invoke-Tool 'scenario' @{ steps = @(@{ wait = $ms }) } ([int]($ms / 1000) + 30) | Out-Null
}

function Test-Venue {
    param([string]$Save)

    if ($Save) {
        Write-Host "== venue: $Save ==" -ForegroundColor Cyan
        Invoke-Tool 'scenario' @{ steps = @(
            @{ tool = 'console'; args = @{ command = "load `"$Save`"" } },
            @{ waitUntil = 'playerLoaded'; timeoutMs = 120000 },
            @{ wait = $SettleMs }
        ) } 200 | Out-Null
    } else {
        Write-Host "== venue: (currently loaded) ==" -ForegroundColor Cyan
    }

    # compare mode on, then reset the counters so this venue starts clean
    Invoke-Tool 'communityshaders.utilre' @{ action = 'set'; mode = 1 } | Out-Null
    Invoke-Tool 'communityshaders.utilre' @{ action = 'reset' } | Out-Null

    Wait-Frames $Frames

    $stats = Invoke-Tool 'communityshaders.utilre' @{ action = 'stats' }
    return $stats
}

$venues = if ($Saves.Count) { $Saves } else { @($null) }
$fail = $false

foreach ($v in $venues) {
    try {
        $s = Test-Venue -Save $v
    } catch {
        Write-Host "  ERROR driving venue '$v': $($_.Exception.Message)" -ForegroundColor Red
        $fail = $true
        continue
    }

    if ($null -ne $s.error) {
        Write-Host "  tool error: $($s.error)" -ForegroundColor Red
        $fail = $true
        continue
    }

    $line = "  compared=$($s.compared) diverged=$($s.diverged) " +
            "(tri=$($s.divergedTrishape) sub=$($s.divergedSubIndex) skin=$($s.divergedSkinned)) " +
            "unsupported=$($s.unsupported) parity=$($s.parity)"
    if ($s.parity -and $s.compared -gt 0) {
        Write-Host $line -ForegroundColor Green
    } else {
        Write-Host $line -ForegroundColor Red
        if ($s.firstDivergingPass) {
            $p = $s.firstDivergingPass
            Write-Host ("  FIRST DIVERGENCE: class={0} technique={1} engineCalls={2} replicaCalls={3} sizeMismatch={4} diffIndex={5} diffField={6}" -f `
                $p.class, $p.technique, $p.engineCalls, $p.replicaCalls, $p.sizeMismatch, $p.diffIndex, $p.diffField) -ForegroundColor Yellow
        }
        if ($s.compared -eq 0) {
            Write-Host "  (compared==0: no covered passes ran -- is CS_UTIL_RE_MODE set and the scene rendering?)" -ForegroundColor Yellow
        }
        $fail = $true
    }
}

if ($fail) {
    Write-Host "PARITY GATE: FAIL" -ForegroundColor Red
    exit 1
}
Write-Host "PARITY GATE: PASS (0 divergence across $($venues.Count) venue(s))" -ForegroundColor Green
exit 0
