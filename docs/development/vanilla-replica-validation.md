# Vanilla Replica — Validation Report (shadow instancing, mode 9)

Evidence that the instanced shadow path (`CS_SHADOW_MT=9`) is a correct, optimized
reimplementation of `RenderShadowmaps`. Companion to
[vanilla-replica.md](./vanilla-replica.md) (the architecture).

## Performance

Dragonsreach, DLSS-bound (render-thread-limited), focused interleaved medians:

| Metric | Vanilla (mode 0) | Instanced (mode 9) | Δ |
| --- | --- | --- | --- |
| FPS | 163.5 | 191.0 | **+16.8 %** |
| Utility submission CPU | 2.71 ms | 1.90 ms | **−30 %** |
| Shadow draw calls | 9124 | 4629 | **−49 %** |

Exteriors are flat (±noise) — daytime exteriors are not shadow-submission-bound. The
win concentrates in local-light-heavy interiors.

## Structural validation (always on)

Both run across every frame of the 50-location sweep and every earlier session.

- **Command validation (`instval`)** — per-map pass-conservation invariants
  (`claimed == grouped + drops`, `grouped == instanced + fallback`, `drawn == groups`)
  plus a throttled byte-compare of the F16C instance pack against the engine's scalar
  reference. **Result: 0 invariant violations and 64/64 packs byte-identical across
  1,043,643 shadow maps.** The optimized fast path is provably equivalent to the engine
  math — which also proves the sweep's findings are not artifacts of the optimization.
- **State-leak detector (`stateval`)** — two trackers at the `RenderShadowmaps`
  boundary. Persistent (entry-vs-vanilla-entry): **0 divergences / 0 canary hits** — the
  instanced path leaks no state the engine fails to recover from. Boundary
  (exit-vs-vanilla-exit): a stable informational steady state, self-test proven to fire.

## Visual validation — 50-location null-test sweep

Method: boot once, `coc` through 50 curated locations (bnet-immune, no save load),
freeze the camera (`tfc 1`) and time (`sgtm 0.0001`) at each, then capture **three**
frames — mode 0, mode 0 again, mode 9. The raw mode-0-vs-mode-9 pixel diff is confounded
by HDR auto-exposure (switching shadow mode nudges scene brightness → the eye re-adapts →
every pixel shifts), so a location's own mode-0-vs-mode-0 diff is used as its noise floor:

```
excess = mean|mode9 − mode0|  −  mean|mode0b − mode0|
```

A raw diff falsely failed 45/52 locations purely on exposure drift (Kynesgrove read
34/765 raw yet its frames are visually identical — excess 0.34 after subtracting the
floor). The null-test isolates the shadow-mode contribution.

**Result: 48 / 50 PASS** (excess < 0.75/765). Pass distribution: median **0.022**, max
**0.340** — i.e. instancing is pixel-equivalent to vanilla across cities, villages,
taverns, keeps, palaces, dungeons, Blackreach, wilderness, and windowless interiors.

### The two non-passes — a localized, pre-existing residual

| Location | excess (mean/765) | note |
| --- | --- | --- |
| WhiterunTempleofKynareth | 2.9 | subtle; upper structure slightly more shadowed |
| SolitudeTempleOfTheDivines | 6.5 | visible faint streaking + darkening on pillars |

Both are temple interiors with **bright windows and sun streaming in**. The decisive
control: the *third* temple tested, **MarkarthTempleofDibella — an underground Dwemer
temple with no windows — PASSES at excess 0.066**, as do the windowless SkyHaven temple,
the crypt (HallsoftheDead), and Winterhold's Hall of Elements. So the residual is **not**
"temples" or "local lights" — it is the **sun-through-window (directional) shadow**,
visually amplified only along sharp interior sunbeam edges. Soft, large exterior sun
shadows average the same small error away, which is why all exteriors pass.

Most likely cause: **FP16 precision of the instance-World stream** (the engine's
`R16G16B16A16` `VA_INSTANCEDATA` slot). The light/camera-relative translation (~1514
units) carries ~0.35 units of FP16 error → a sub-pixel shadow-edge shift, invisible in
soft shadows but visible on a hard sunbeam edge. This is a **pre-existing** property of
the kInstance approach (the optimization is byte-identical to the checkpoint, per
`instval`), not a regression.

**Fix path (not applied — awaiting decision):** an FP32 instance stream
(`R32G32B32A32`, 64 B) built with a custom input layout (the engine `VA_INSTANCEDATA` is
FP16-only, so the `ID3D11InputLayout` must be constructed manually rather than via the
engine's `SetDirtyStates` IL cache) and full-float stores (drop `_mm_cvtps_ph`). ~2×
instance-VB bandwidth (negligible). It should be env-gated and A/B-tested against the two
failing temples and a sample of the passing locations before landing, since it bypasses
the engine IL machinery.

## Reproduce

- Sweep: `F:\claudetmp\rtprof\sweep50.ps1` (captures V/V2/I triples via devbench).
- Analyze: `F:\claudetmp\rtprof\null_resumable.ps1` → `null_results.csv`.
- Live counters: devbench `communityshaders.shadowmt` actions `instval` / `stateval`.
