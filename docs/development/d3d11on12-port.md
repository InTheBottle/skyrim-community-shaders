# D3D11On12 Port (feat/d3d11on12)

Replace the DXVK/Vulkan stack with Microsoft's open-source D3D11On12 mapping layer
(doodlum/D3D11On12 fork, branch `cs-integration`): the game's D3D11 runs on a CS-owned
**native D3D12** device. No Vulkan anywhere. Streamline, FidelityFX and XeSS all use their
**first-class D3D12 backends**, which removes the entire class of Vulkan-interposer problems
the DXVK branch fought (present-path flip locks, semaphore contracts, the DLSS-G flash,
alt-tab wedges).

## Architecture

```
Skyrim D3D11 calls
   └─ system d3d11.dll runtime (API front-end, COM objects)
        └─ DDI driver: OpenAdapter_D3D11On12  ← exported by CommunityShaders.dll
             └─ D3D11On12 layer (STATIC LIB, linked into CS)      [doodlum/D3D11On12 @ cs-integration]
                  └─ D3D12TranslationLayer (static)               [microsoft/D3D12TranslationLayer]
                       └─ CS-owned ID3D12Device + direct queue
                            └─ DXGI flip-model swapchain (native) ── Streamline D3D12 proxy owns present
```

- **Static lib, not a DLL** (user requirement): build `d3d11on12` + `d3d12translationlayer`
  as static libraries linked into CommunityShaders.dll. The system `d3d11.dll` runtime
  resolves the DDI provider with `LoadLibrary("d3d11on12.dll")` + `GetProcAddress
  ("OpenAdapter_D3D11On12")`; CS exports that symbol and Detours the runtime's LoadLibrary
  call for that one name to return CS's own HMODULE (same Detours infra as the existing
  hooks).
- **Device creation**: the `D3D11CreateDeviceAndSwapChain` hook creates the D3D12 device +
  direct queue, then `D3D11On12CreateDevice(...)` (system API) for the game-facing
  ID3D11Device. The swapchain is a native DXGI flip-model swapchain on the D3D12 queue,
  created through Streamline's proxy factory so `sl.dlss_g` owns present — the supported
  SL integration, not an interposer hack.
- **Resource interop**: D3D12 resources for Streamline tags come from the layer's
  unwrap surface (`external/d3dx11on12.h`) — no cross-API import, no layout translation,
  no image-view caches. Evaluates are recorded on CS-owned D3D12 command lists on the same
  queue/timeline as the game's translated work.

## What dies (Vulkan surface, all of it)

| DXVK-branch piece | Fate |
| --- | --- |
| `src/DxvkLoader.*` (dxvk d3d11 loading, vulkan-1 interposer arrangement) | replaced by D3D12 device creation + DDI redirect |
| `src/Features/Upscaling/DxvkInterop.*` (VK queue/cmd/semaphore interop) | replaced by a thin D3D12 command-list helper |
| dxvk exports @100–@108 (FG ownership, FSE pNext, latency skip, present-wait…) | all unnecessary on native DXGI |
| Streamline VK paths (VkImageView caches, optical-flow probe via vulkan-1, per-window flip-lock logic, the forced CPU present bound) | D3D12 tags; probe via D3D12 feature/NVAPI; DLSS-G D3D12 pacing is the shipped-everywhere path |
| FSR-FG VK backend in sl.fsr; XeSS VK gating | D3D12 backends (Streamline fork branch `feat/d3d12-backends`); XeLL/XeFG (D3D12-only) become available |

## Phases

1. **Standalone build**: 11on12 + TranslationLayer build as static libs (needs WDK for
   `d3d12translationlayer_wdk`; verify local WDK, else vendor the few WDK headers).
   CMake change on `cs-integration`: `SHARED` → `STATIC`, keep the `.def` for reference.
2. **CS scaffolding**: submodule `extern/D3D11On12` (fork, `cs-integration`); CMake links
   the libs; export `OpenAdapter_D3D11On12`; LoadLibrary redirect; feature flag to boot
   11on12 vs dxvk during bring-up.
3. **Boot the game** on 11on12 with upscaling features disabled; fix translation-layer
   gaps Skyrim hits (the DXVK campaign's map of Skyrim's D3D11 quirks is the checklist).
4. **Streamline D3D12**: device registration, tags (depth/MV/hudless as ID3D12Resource),
   DLSS/DLSS-G through the SL DXGI proxy swapchain; FSR-FG/XeSS via the fork's new D3D12
   plugin backends; hardware-derived method selection stays.
5. **Parity + stability**: flash check (expected clean — D3D12 is SL's mainline), alt-tab
   (native DXGI occlusion handling), FSE/borderless matrix.
6. **VTune campaign** on the 11on12/TranslationLayer fork (the DXVK campaign's method:
   auto-load harness, interleaved A/B, fix the top CPU hotspots — expected: batched
   command translation, descriptor churn, resource-state tracking).

## Risks / open questions

- **WDK dependency** of D3D12TranslationLayer on this machine's toolchain (VS2026 preview).
- The translation layer may need its own fork for VTune fixes → ask doodlum to fork
  microsoft/D3D12TranslationLayer when phase 6 starts.
- 11on12 completeness vs Skyrim's D3D11 usage (deferred contexts? `Map` patterns?
  DXVK handled these; the translation layer is Microsoft-maintained but less game-tested).
- Streamline D3D12 + the existing hudless/present hook ordering — redesign around the SL
  proxy swapchain rather than the dxvk present callback exports.

## Phase 4 design — Streamline D3D12 (drafted after the phase-3 milestone)

Boot order in the shim (`src/D3D11On12Loader.cpp`):
1. `slInit` (manual-hooking flags, D3D12) **before** device creation; feature set from the
   hardware probe (D3D12 feature checks/NVAPI replace the Vulkan optical-flow probe).
2. Create the D3D12 device → `slSetD3DDevice(d3d12Device)`.
3. Swapchain through Streamline's proxied factory (`slUpgradeInterface` on the DXGI factory
   before `CreateSwapChain`) so `sl.dlss_g` owns present — the mainline D3D12 pacing path.
4. `D3D11On12CreateDevice` on top, as today.

Per-frame integration:
- **Tags**: QI the game device for `ID3D11On12Device2`;
  `UnwrapUnderlyingResource(depth/MV/hudless, queue)` → `ID3D12Resource` for
  `slSetTagForFrame`, `ReturnUnderlyingResource` after the evaluate is recorded. Flush the
  11on12 immediate context before SL evaluates on our own D3D12 command list so translated
  writes are queue-ordered ahead (same queue).
- **Evaluates** (DLSS/DLSS-G/FSR/XeSS): CS-owned `ID3D12GraphicsCommandList` submitted to the
  shared queue; no cross-API interop, no image-view caches, no layout pinning.
- **Reflex/PCL**: unchanged marker API on D3D12.
- The whole `DxvkInterop` + present-bound + window-gap machinery does not carry over; DXGI
  occlusion (`DXGI_STATUS_OCCLUDED`) handles minimize natively.

Streamline fork (`feat/d3d12-backends`): `sl.fsr`/`sl.xess` gain D3D12 device support beside
Vulkan (FFX + XeSS both ship native D3D12 backends; the plugin work is context creation +
resource plumbing per API, keeping the existing options/tuning surface).
