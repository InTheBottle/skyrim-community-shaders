# Failure triage procedures

## Crash triage

1. **Find the artifact.** `C:\Users\timpy\OneDrive\Documents\My Games\Skyrim Special
   Edition\SKSE\crash-YYYY-MM-DD-HH-MM-SS.log` (CrashLogger). Match its timestamp to the
   session you launched — a stale crash log from an earlier session is a classic false lead.
2. **Read the header**: exception type, faulting instruction, and the register analysis
   (`rcx = 0x0 (likely invalid)` tells you it's a null deref before you read a single frame).
3. **Read the stack** (`CALL STACK` section). Frames look like
   `SkyrimSE.exe+0D71DB2 -> 75590+0x92`. The number before `+0x` is an Address Library ID.
4. **Resolve IDs to engine names**: `grep -rn "<ID>" extern/CommonLibSSE-NG/src
   extern/CommonLibSSE-NG/include` — CommonLib wraps most engine functions with
   `RELOCATION_ID(<SE-ID>, <AE-ID>)` next to a readable function name
   (e.g. 75445 → `BSGraphics::Renderer::Init`).
5. **CS frames** (`CommunityShaders.dll+offset`) symbolicate with the PDB:
   `cdb -z <dump> -y <worktree>\build\ALL\Release` or by matching the offset in a map file.

**Why not just attach a debugger?** CrashLogger installs a vectored exception handler that
logs and terminates before a debugger's second-chance handling gets anything useful; an
attached `cdb` typically reports only `Exit process ... code c0000005`. Use the CrashLogger
output as ground truth; use `cdb` for *hangs*, where there is no exception to race for.

## Freeze / wedge triage

1. Confirm it's a wedge, not a load: process CPU near zero + not `Responding` + present rate 0
   (PresentMon returns an empty CSV — the `@()` guard matters).
2. Capture stacks BEFORE killing anything:
   ```powershell
   & "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe" -pv -p <pid> `
     -y "<worktree>\build\ALL\Release" -c "~*k 6; qd" | Out-File wedge_stacks.txt
   ```
   `-pv` is non-invasive (the wedged process stays wedged, but you can read it).
3. Look for: a thread in `NtDxgkSubmitPresentToHwQueue` or a driver wait (present-path
   wedges), two threads holding each other's locks, or a CS/Streamline frame blocked on a
   fence that will never signal.
4. Kernel-present wedges (present submitted, never retires): no app-side fix exists; document
   and move the investigation to ETW / vendor-report territory.

## Vulkan validation layer (DXVK-era diagnosis; historical but reusable)

- Dev Streamline DLLs route validation-layer messages into `CommunityShaders.log`. Stage the
  dev set from the SDK's `bin/x64/development` next to the production DLLs (back the originals
  up to `_production_backup` first), then put `sl.interposer.json` with
  `{"vkValidation": true, "logLevel": 2}` in the **game root** (exe directory).
- The loader must be able to find a modern `VK_LAYER_KHRONOS_validation`: the registry on this
  rig points at a stale SDK on the full C: drive; `VK_LAYER_PATH=H:\VulkanSDK\Bin` fixes
  standalone apps. The full game + validation combination never booted here — validate with
  the Streamline sample instead when possible.
- **Always delete `sl.interposer.json` and restore production DLLs afterward** — the leftover
  config crashes every normal launch (this happened; the user lost a session to it).

## In-game A/B harness pattern

For each variant: build (hash-proof) → deploy (hash-proof) → launch (protocol) → autoload the
same save via devbench → soak N seconds → PresentMon sample → kill (safety rule 2) → next
variant. Interleave variants; the rig warms ~20 fps per session. Two clean sessions minimum
before declaring an intermittent bug fixed — session-to-session variance has manufactured
false "fixed" verdicts more than once.

## Environment gates reference

| Env var | Effect |
| --- | --- |
| `CS_D3D11ON12=1` | Boot the embedded D3D11On12 / native-D3D12 path instead of DXVK |
| `CS_FORCE_FSR_FG=1` | Force the FSR frame-gen path on DLSS-G-capable hardware |
| `CS_SL_VERBOSE=1` | Streamline-internal logging into CommunityShaders.log |
| `VK_LAYER_PATH` | Point the Vulkan loader at a specific validation-layer directory |

A user-facing launcher exists at `<game>\Launch_D3D11On12_Test.bat` (sets `CS_D3D11ON12=1`).
