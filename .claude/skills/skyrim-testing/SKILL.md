---
name: skyrim-testing
description: >-
  Automated build-deploy-launch-verify-triage loop for testing Community Shaders in a real
  Skyrim SE instance on this rig. Use this skill whenever a change needs in-game validation —
  building and deploying the CS DLL or AIO, launching the game, checking that a feature engaged
  (DLSS/DLSS-G/FSR frame generation, upscaling, the D3D11On12 path), measuring FPS or present
  mode with PresentMon, loading a save for in-world testing, diagnosing a crash/freeze/wedge, or
  running an A/B performance comparison. Trigger on phrases like "test this in game", "deploy
  and check", "does it still boot", "did frame gen engage", "why did it crash/freeze", "measure
  the fps difference" — and any time you are about to launch, kill, or redeploy over a running
  game (the safety rules below are load-bearing; violating them has crashed user sessions and
  destroyed a game install).
---

# Automated Skyrim testing (Community Shaders)

The full loop is **build → verify the build → deploy → verify the deploy → launch → wait →
verify engagement → (optionally) go in-world → measure → triage**. Every step has a
verification because every step has silently failed before: builds that error without printing
errors, deploys that never happened, launches that tested a stale DLL. The single most
important habit in this skill: **never interpret a test result until you have proven which
binary produced it.**

## Machine facts (this rig)

| Thing | Where |
| --- | --- |
| Game install (SE 1.5.97, Steam) | `I:\SteamLibrary\steamapps\common\Skyrim Special Edition` |
| Deploy target | `<game>\Data` |
| Launcher | `<game>\skse64_loader.exe` |
| CS log | `C:\Users\timpy\OneDrive\Documents\My Games\Skyrim Special Edition\SKSE\CommunityShaders.log` |
| Crash logs | same SKSE folder: `CrashLogger.log`, `crash-YYYY-MM-DD-*.log` |
| AIO build output | `<worktree>\build\ALL\aio` (DLL at `aio\SKSE\Plugins\CommunityShaders.dll`) |
| Build scripts | `F:\claudetmp\build_aio.bat` (CS AIO), `F:\claudetmp\build_dxvk.bat` (dxvk via ninja — `build_aio.bat` does NOT rebuild dxvk), `F:\claudetmp\build_11on12.bat` (D3D11On12 static libs) |
| PresentMon | `F:\claudetmp\tools\PresentMon.exe` |
| PDB symbols for debuggers | `<worktree>\build\ALL\Release` |
| devbench REST | `http://127.0.0.1:8920/api/tool` (available once the game is up; no MCP handshake needed) |
| GPU | RTX 4080 (DLSS-G capable) |

## Safety rules (each one earned the hard way)

1. **Never `robocopy /MIR` into the game Data folder.** It deletes everything not in the
   source — it has destroyed this game install twice. Always `/E`.
2. **Kill only the game's own process**: `Get-Process SkyrimSE | Where-Object { $_.Path
   -notlike "F:*" }`. The Streamline sample sometimes runs AS `SkyrimSE.exe` from `F:` for
   driver-profile parity; killing it destroys a separate experiment.
3. **Never kill, redeploy, or relaunch while the user may be testing in-game.** If a session
   was handed to the user (or they report being in-game), the game is theirs until they say
   otherwise. Deploy work waits.
4. **Remove diagnostic artifacts when done.** A leftover `sl.interposer.json` (validation
   config) in the game root makes every normal user launch crash at `vkCreateInstance`.
   Anything staged for diagnosis (dev DLLs, validation configs, modified INIs) gets restored
   from its `_production_backup` before handing the game back.
5. **Run `.bat` files via the PowerShell tool** (`& cmd /c "F:\claudetmp\x.bat"`). Invoking
   them through bash→cmd quoting opens an interactive cmd that hangs the task forever.

## The loop

### 1. Build, with hash proof

```powershell
$dll = "<worktree>\build\ALL\aio\SKSE\Plugins\CommunityShaders.dll"
$before = (Get-FileHash $dll -Algorithm MD5).Hash.Substring(0,8)
& cmd /c "F:\claudetmp\build_aio.bat" 2>&1 | Select-String -Pattern "error C|LNK\d|CMake Error|FAILED" | Select-Object -First 8
$after = (Get-FileHash $dll -Algorithm MD5).Hash.Substring(0,8)
"CS: $before -> $after"
```

An unchanged hash means the build did NOT produce your change, even when no error text
appeared — namespace-scope compile errors, private-member access, and missing includes have
all failed silently here. Treat `before == after` as a failed build and go find the error.
C++-only fast path when the AIO packaging isn't needed: `cmake --build build/ALL --target
CommunityShaders --config Release` and copy the single DLL.

### 2. Deploy, with hash proof

```powershell
robocopy "<worktree>\build\ALL\aio" "I:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data" /E /NFL /NDL /NJH /NJS
(Get-FileHash "I:\...\Data\SKSE\Plugins\CommunityShaders.dll" -Algorithm MD5).Hash.Substring(0,8)
```

The deployed hash must equal the build hash. A whole debugging afternoon was once spent on
results from a stale deployed DLL because a launch-only script was mistaken for a
deploy-and-launch script. robocopy exit codes 1–3 are success (files copied), not errors.

### 3. Launch, with the timing protocol

Set any path-selecting env vars in the same PowerShell session **before** launching (children
inherit them): `CS_D3D11ON12=1` (D3D11On12/native-D3D12 path), `CS_FORCE_FSR_FG=1` (force
FSR-FG on DLSS-G hardware), `CS_SL_VERBOSE=1` (Streamline-internal logs into the CS log).

```powershell
Start-Process -FilePath "<game>\skse64_loader.exe" -WorkingDirectory "<game>"
```

- Wait for the main menu **and at least 85 seconds**, then **hold 30 more seconds** before
  loading a save — loading earlier intermittently crashes the Bethesda.net services boot.
- Leave **~2 minutes of cool-down** between relaunches.
- Long waits happen in a `run_in_background` command with the verification baked in, never by
  polling.

### 4. Verify engagement before believing anything

Three independent signals, all three every time:

1. **Process**: alive and `Responding` after 60s.
2. **Log**: grep `CommunityShaders.log` for the feature's marker (examples:
   `DLSS-G mode=true`, `[D3D11On12] game device online`, `framesPresented=2` is the only
   reliable FSR-FG engagement signal). Log lines have a 26-char timestamp prefix; substring it
   off for readable output.
3. **PresentMon** for present rate and path:
   ```powershell
   & F:\claudetmp\tools\PresentMon.exe --process_name SkyrimSE.exe --output_file out.csv --timed 8 --terminate_after_timed --stop_existing_session
   $csv = @(Import-Csv out.csv -ErrorAction SilentlyContinue)   # @() guards the empty-capture case
   "$([Math]::Round($csv.Count/8)) fps ($($csv[0].PresentMode))"
   ```
   `Hardware: Legacy Flip` / `Hardware Composed: Independent Flip` = healthy flip presents
   (frame generation doubles visibly in this number). `Composed: Copy with GPU GDI` = a broken
   present path; nothing downstream of presentation can be trusted. Frame generation ON should
   show roughly double the engine frame rate.

### 5. In-world testing (no user needed)

The devbench REST bridge drives the game once it is up:

```powershell
Invoke-RestMethod -Uri "http://127.0.0.1:8920/api/tool" -Method Post -ContentType "application/json" `
  -Body (@{ name = "console"; args = @{ command = 'load "<savename>"' } } | ConvertTo-Json)
```

- Issue `load` only after the full launch protocol above (menu + 85s + 30s hold).
- `feature-set` REPLACES the entire settings block for a feature — sending a partial object
  resets everything else in it to defaults. Read-modify-write.
- `coc` via console pauses the game clock — do not use it for perf measurements.
- Poll profiler/inspect endpoints sparsely; hammering them perturbs frame times.

### 6. Measure (perf verdicts)

- **The rig warms up ~20 fps across a session.** A/B comparisons are only valid interleaved
  (A,B,A,B — never all-A then all-B) or in the same session.
- **Intermittent bugs need ≥2 clean sessions before a "fixed" verdict.** One clean session has
  produced false confidence repeatedly (the DLSS-G flash saga).
- Prefer FPS measured from frame-counter deltas over eyeballing an overlay.

### 7. Triage failures

Read `references/triage.md` for the full procedures. The short version:

- **Crash**: `crash-*.log` (CrashLogger) is the primary artifact — its exception handler wins
  the race against an attached debugger, so an attended `cdb` usually sees only the process
  exit. Resolve the `SkyrimSE.exe+offset -> <RE-ID>+0x..` IDs by grepping the ID number in
  `extern/CommonLibSSE-NG` to get the engine function name.
- **Freeze/wedge**: attach `cdb -pv -p <pid> -y <worktree>\build\ALL\Release -c "~*k 6; qd"`
  and read which thread holds what. Kill the process only after capturing stacks.
- **"It does nothing"**: 90% of the time the wrong binary ran — re-verify build hash, deploy
  hash, and env vars before theorizing.
