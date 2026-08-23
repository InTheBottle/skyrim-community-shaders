# Skyrim 1.7.99 graphics port

This document records the evidence, implementation decisions, and validation for
the Skyrim 1.7.99 Community Shaders port. Results are recorded only after they
have been reproduced; open questions are kept in the final section.

## Scope

- Add Skyrim 1.7.99 support without regressing 1.5.97 or 1.6.1170.
- Compare the complete 1.6.1170 and 1.7.99 vanilla shader sets and the relevant
  renderer code paths. Independently sourced 1.5.97 shader assets remain a
  separate validation item.
- Keep one compiled Community Shaders shader set. The 1.7 shader/CPU contract
  is authoritative; only older runtimes receive compatibility adapters.
- Backport applicable official graphics fixes to the older supported runtimes.
- Audit upscaling and shader-cache behavior in particular.
- Test with Steam closed by launching Steamless executables directly.
- Temporarily allow Community Shaders to run without Engine Fixes while an
  updated Engine Fixes build is unavailable.

## Baseline

- Repository: `community-shaders/skyrim-community-shaders`
- Base branch: `dev`
- Base commit: `89ce43ea9`
- Working branch: `support/skyrim-1.7.99`
- Configuration: `Dev-Fast`, SE/AE/VR enabled, DevBench bridge enabled
- Result: clean baseline build passed on 2026-08-22
- DLL: 32,941,568 bytes
- DLL SHA-256:
  `1DAEFE7C7B6A3DBC5724F7BAB19604C61BCAE7AC2D6DA967A8E7AA96B89CEDB3`
- PDB SHA-256:
  `7D798F0A0F27C095D5A2452A7359F69E878331DD8A640326D7199A4F012A24DF`

Build artifacts are kept in a local gitignored build directory so generated SDK
and object data do not enter version control.

## Verified compatibility blockers

1. The original CommonLibSSE-NG revision was `8f4205da5` (`v4.26.1`). Its
   `SKSE/Version.h` constants end at Skyrim 1.6.1170 and its AE Address Library
   reader expects the pre-1.7 format. It therefore cannot load the 1.7.99
   format-5 database.
2. The original `src/XSEPlugin.cpp` required `EngineFixes.dll`. A missing DLL
   added a fatal compatibility error and prevented all Community Shaders hooks
   and features from being installed.
3. The plugin metadata calls `UsesAddressLibrary()` and `UsesNoStructs()`.
   Those declarations required independent evidence for every direct engine
   field and relocation rather than relying on the CommonLib update alone.

## CommonLibSSE-NG migration

The submodule now points to the Community Shaders CommonLibSSE-NG fork at
commit `0bd0be90ef5a14636c9c27b8358e6b0bcfe41b53`. It is based on the released
`v6.6.0` commit `8b48fb1b76d6ce9353af138d86037b4246c73527`, retaining its Skyrim 1.7.99
Address Library format-5 support, with a binary-audited `BSGraphics::State`
correction for this port. The submodule URL follows the fork so a fresh clone
can resolve that commit.

The base revision predates [CommonLibSSE-NG PR
306](https://github.com/alandtse/CommonLibSSE-NG/pull/306), which was
subsequently merged on 2026-08-22. Community Shaders does not call any API
changed by that PR (`SkyrimVM::GetImpl()` or the three `PlayerCharacter`
event-source accessors), so those later declarations are not a dependency of
this port.

The 1.7 additions do not replace the older layouts. Runtime-version accessors
now distinguish all three flat desktop layouts: the `State` runtime tail begins
at `+0x58` on Steam SE 1.5.97, `+0x60` on AE 1.6.1170, and `+0x70` on AE
1.7.99. `frameCount` remains at `+0x4C` on the two older runtimes and moves to
`+0x54` on 1.7.99. The new 16-byte 1.7 prefix at `+0x4C` is exposed separately
instead of being folded into the common tail. The public double-dynamic-
resolution-frequency accessor selects AE `+0x57` or 1.7 `+0x63`; the latter is
a bounded structural inference from the independently witnessed `+0xC` prefix
migration, not a claimed direct xref. SE and VR return `false`.

This preserves one SE/AE/VR plugin binary while keeping 1.7 authoritative at
runtime. The retained v6.6.0 VR declaration passes its isolated compile gate,
but this port did not independently prove the VR prefix against a game binary.

Four source adaptations were necessary for the newer public declarations:

- Volumetric-light RGB components are now accessed through the named `color`
  member rather than three flattened members.
- The SSAO toggle now uses `SAOEffectParams::enableSAO` instead of pointer
  arithmetic to byte `0x50`.
- The phantom `ISGraphicsTextureFilterMode` image-space descriptor was removed;
  CommonLibSSE-NG documents the corresponding VR slot as
  `ISReflectionBlurHCS`.
- The private `src/ShaderTools/BSShader.h` ABI overlay was removed. Its
  `BSShader` and `PixelShader` sizes and field offsets were independently
  verified in all three Ghidra programs, then users were migrated to v6.6.0's
  canonical `RE::BSShader` and `RE::BSGraphics::PixelShader` declarations.
  The only C++ type-system boundary is an explicit cast from the global D3D11
  pixel-shader interface to CommonLib's ABI-equivalent `REX::W32` interface.

The multi-runtime `Dev-Fast` build passed after these adaptations on
2026-08-22. The resulting DLL is 32,946,176 bytes with SHA-256
`0553F75E0A990A1626001D3B9FA8CA50780545C859592773C38D0B502A6FC2DB`;
the PDB SHA-256 is
`F89B10C426ACBCCB7E20089B1D8D8EF867F9FE35E93B62119329B5146D768B9A`.

An independent source build of the v6.6.0 base and its complete SE/AE/VR test
target passed 47/47 tests. This covers Address Library
format-1, format-2, and format-5 loading and auto-detection, backward format-2
compatibility, 1.7.99 module recognition, cross-runtime relocation/accessor
selection, and loud failure for unsupported database formats. The test copies
of the 1.5.97, 1.6.1170, and 1.7.99 databases are byte-for-byte identical to
the local Address Library corpus used by the Ghidra imports. Their SHA-256
values are respectively
`1D7530D001139CA58F462EA0210A8055868159057BA8B5EBC624FC5E9C4F5E9A`,
`C4093C569A3C83B26587F4B9EA4C55DE9AE6E73B84A2AF9FB3FBD30E2FE0D452`,
and `184FCA0C834E0D2523B450D18EA32C9FBF9F6295E88E936712B7360F1FCCC4EC`.

A second standalone verifier linked the exact v6.6.0 `CommonLibSSE.lib`, loaded
only the production format-5 database, independently validated every RVA
against the 1.7.99 executable image, and passed ten exact ID-to-RVA probes. It
reports format 5, version 1.7.99.0, 565,073 entries, and 435,154 populated
entries. The verifier result record has SHA-256
`55B15DBF978312BFC164A734EB23672EECBA0F4A3C0A7663B6E6FAD4C3652052`.

The corrected fork subsequently passed isolated `ALL`, AE-only, SE-only, and
VR-only source builds. The VR result is a structural compile check, not binary
semantic proof; its verification record has SHA-256
`9308797EC235B087ADB7F86B640051A458F9C4E272D48545D1705B746BDA42F5`.
The three-desktop-runtime `State` binary report has SHA-256
`BCC98FD2C54CEEF35C57F27E5AC872A920185D724B3AB7FBAB3A70BC28FD25E0`.
Its corrected `State` reference counts are 390/429/432 for Steam
1.5.97/1.6.1170/1.7.99, with zero unresolved. The frozen State/Legacy
reconciliation has
SHA-256
`077CE8775359C8FAC8EEA4D4110E24CE1ABF2B48DE88BBA83A78BF3AB906593E`;
it records all four active adapter families and the absent, dormant-gated
`Renderer::Begin` transplant.

The temporary external Engine Fixes presence check has been removed. Community
Shaders' own `src/EngineFixes` implementations and every unrelated incompatible
DLL check remain enabled.

## Structure and Address Library evidence

Full same-fork validation of commit `0bd0be90e` is complete. Community Shaders' production
`ALL` preset selects the SE+AE+VR `SKYRIM_CROSS_VR` wrapper model, so the audit
imported both that compile-time model and the exclusive SE/AE flat declaration
models into isolated Ghidra programs for Skyrim 1.5.97, 1.6.1170, and 1.7.99.
All six transactions committed and saved with zero import errors and zero
post-reopen mismatches. The full import audit has SHA-256
`3EDE29522AEEA8A14425FEE31D18DC2D47392B4FF657E47B4A98C953A467F958`;
its `ALL` and exclusive-flat manifests have SHA-256 values
`62862A49B439CD6543AFE191A8C7B6DAD9A54FF82A772A6D7B441ADE65152FE8` and
`B8A5D9D9BDDFF45986BBB104663D1DD59129A4770A0A441E17E9533122259575`.

An `ALL` declaration is a callable multi-runtime wrapper, not necessarily a
whole-object byte-for-byte engine overlay. The one importer correction was the
binary-derived `Actor` primary-vtable anchor: all three binaries place
`KillDying` at slot `0xAA` and the distinct `Resurrect` at `0xAB`. Its evidence
has SHA-256
`DA897CB6602E5A929D43260AA2470C3E87760472271BC5C87B27A26C7A2D2B5F`.

The compiler-bound field audit was regenerated from the frozen production
source, including the new legacy graphics module. All 137/137 translation
units passed with zero malformed records. It found 8,596 raw matches, 3,078
deduplicated source accesses, and 504 unique `RE` `FieldDecl` identities.
Stable direct-prefix fields remain direct. Versioned fields are
classified by the accessor that reaches them, including `State::GetFrameCount`
at `+0x4C/+0x4C/+0x54` and `State::GetRuntimeData` at
`+0x58/+0x60/+0x70` for Steam 1.5.97/1.6.1170/1.7.99. The two
`ExtraDataList` members retain stable outer addresses and are used only through
version-aware `GetByType`. No unsafe or unresolved 1.6.1170-to-1.7.99 direct
field access remains; the audit found zero changed direct 1.6-to-1.7 pairs.
The classification covers all 57 `State::RUNTIME_DATA` accesses and catalogs
15 CommonLib 1.7 headers with 44 runtime-gate occurrences.
The reconciled audit closes 504/504 identities (502 invariant and two compact-
wrapper cases) with zero unsafe or unresolved uses. The compiler extraction,
reconciled field audit, and accessor classification have SHA-256 values
`FCBA3F17B5BB5B7984F7FF9C8DC1F93F357EC75BCB46E79C7BCC45BC47E81EA9`,
`817117B320C9AC20233154B805F8D688D60D22E421D4C012B10EE4B522E736BB`, and
`399B7D519556EBF31B89E254D8885829FCED529E9D9BCBE30B7DB74719B7594B`.

Accordingly, legacy `UsesNoStructs()` (the old export spelling of
`StructCompatibility::Independent`) is defensible; it does not literally mean
that the plugin never names an `RE` field. A standalone, non-loading PE verifier
also reproduces the exact loader matrix: SE 1.5.97/SKSE 2.0.20 and VR/SKSEVR
2.0.12 use the legacy `Query` path, while AE 1.6.1170/SKSE 2.2.8 and AE
1.7.99/SKSE 2.3.0 accept Address Library plus Independent when the matching
database exists. The matrix has SHA-256
`4FC85BAF50C235D96F038709345137D90193847202A7D269F8C40A3C057E13BC`.

The frozen source contains 124 unique paired Address
Library ID tuples: 85 executable relocations and
39 data globals. Every executable target is either an equivalent native role or
an exact-preflight forward adapter target. All 39 data globals retain
independent semantic proof: 13 were already closed, four are exact
`RE::Setting` records, and 22 were recovered by exhaustive executable-section
decoding and matched native field-use/callsite fingerprints. The decoder
covered 6,651,975 SE 1.5.97, 7,310,569 AE 1.6.1170, and 7,463,500 AE 1.7.99
instructions without using numeric ID continuity as semantic proof. The frozen
direct-relocation semantic audit has SHA-256
`F82D6B363BAE855F6C01CAE4C980C0FD341468B8203A584D8EBCFBC5DA0B6D45`;
its reviewed 124-tuple aggregate closure has SHA-256
`79B3424B01B0BE1A1BADD68E2224DB26D7922AA700F2D620A3044C0FD76AB333`.

That pass also identified ID pair `513510/391362` as the one-byte
`bIBLFEnable:Display` setting. The hook now writes it through `bool*`, rather
than the previous `float*`. The final shadow-distance pair `528314/415263` is
closed by nine shared native callsites plus the new 1.7 writer at ID 527757;
both versions write the distance and its square at the adjacent float. The full
data-global report has SHA-256
`C4A4BC824A06F4C3A81FB8EB4C5E5BE8315491953D52C6D7382F241F35686533`.

The compact machine-readable compatibility verdict records 6/6 full imports,
504/504 field identities, 68/68 interior sites, 294/294 vtable pairs, 124/124
direct semantic relocation closures, 6,998/6,998 shader semantic keys, zero
actionable outside-graphics gaps, and zero unresolved static mappings. Its
SHA-256 is `7A2DAD774AEB1EF7128B9F111CDD49D4B9F7490BA4A55FF0F689C65C28E454C7`.
The deterministic post-freeze evidence manifest pins 33 scripts and 47
artifacts with a seven-stage exact rerun sequence; its SHA-256 is
`81EF1603B3F070CC052892BF423AF2748EBA12D3BD9FEB7FE0E9232C87EB5661`.

## Runtime inventory and no-Steam gate

- Skyrim 1.6.1170 Steamless executable SHA-256:
  `8F2116B073D5D74713B39A2B11CD35E6B745A760E690E1F46FC7AE76A9C0F48A`;
  paired with SKSE 2.2.8.
- Skyrim 1.7.99 Steamless executable SHA-256:
  `0B473F0D6C42D0B2885266E78394A64C980480E9663DD1EA8B51731961D0C18A`;
  paired with SKSE 2.3.0.
- The installed signed 1.7.99 executable has SHA-256
  `B34E3655489DD655EB12B8221E24F8CE38524ED4E292E07BF3B977CDB488DAAA`
  and must never be used by the unattended test launcher.
- The 1.6 and 1.7 snapshots contain different SKSE script releases, so the
  executable, loader/runtime DLLs, shader archive, and complete `Data/Scripts`
  tree must be switched as one transaction.

Runtime automation must refuse to launch while `steam`, `steamservice`, or
`steamwebhelper` is present; accept only the exact Steamless hashes above; and
monitor for Steam starting during a run. A signed executable is rejected as a
second line of defence. A coherent 1.5.97 snapshot has not yet been assembled,
so no 1.5 runtime result will be claimed until that is resolved.

External transactional tooling was created in an isolated, non-repository QA
workspace. The switcher is dry-run-first, stages and hashes every changed file,
backs up replacements, quarantines conflicting runtime files, synchronizes the
124 SKSE-owned script files without deleting unrelated mod scripts, journals
every mutation, and supports guarded rollback. The launcher is check-only by
default, exact-hash gated, and combines `Win32_ProcessStartTrace` with 100 ms
polling. It fails closed if process-start monitoring cannot be armed. All 22
safety assertions and both source-snapshot dry runs pass. The dated local
preflight found running Steam and Skyrim processes, so no launch, switch, or
process termination was attempted.

## Runtime-scoped shader storage

The original dump path (`Data/ShaderDump/<loader>/<descriptor>`) and disk-cache
path (`Data/ShaderCache/<shader>`) contained no game-runtime identity. Switching
between 1.6 and 1.7 could therefore overwrite a dump with the same descriptor
or consume compiled cache data produced for a different executable.

Dump and cache storage are now namespaced by the exact Skyrim runtime and a
24-hex (96-bit) prefix of the running executable's SHA-256. Full executable
hashes remain authoritative in cache metadata and dump manifests:

- `Data/ShaderDump/<runtime>/<exe-sha256-prefix-24>/s-<session-id-24>/...`
- `Data/ShaderCache/<runtime>/<exe-sha256-prefix-24>/...`

Each vertex/pixel dump path contains a 12-character readable loader prefix, a
24-hex loader-name SHA-256 prefix, the descriptor, stage, and a 24-hex bytecode
SHA-256 prefix. The append-only JSON-lines manifest retains the unsanitized
loader name and the full executable and bytecode SHA-256 values, together with
the runtime, session, descriptor, stage, source, bytecode size, and relative
path. Existing content is checked against the full hash; a truncated loader or
bytecode prefix collision never overwrites data and permanently invalidates the
session. Writes are hashed after completion, missing bytecode captures are
reported rather than throwing, and capture/dump maps are synchronized for
concurrent shader creation.

Manifest records are marked complete only after an explicit write, flush, and
close. A failed append is rolled back and remains retryable. If rollback fails,
`manifest.invalid` is written and further manifest appends are disabled for the
session while shader binaries remain available for diagnosis.

Cache metadata schema 2 records the runtime, full executable hash, plugin
version, and feature settings. The full identity must match before any cache in
the shortened directory scope is accepted, so a directory-prefix collision
cannot cause stale reuse. Validation and invalidation affect only the active
runtime/executable scope; a legacy unscoped cache is logged and ignored. Failure
to hash the executable generates a unique per-process identity, deliberately
disabling reuse instead of risking a cross-version cache hit.

`tools/verify-runtime-shader-storage.py` independently validates shortened path
structure against full metadata, every manifest path, size, and SHA-256, and
compares two verified sessions by `(loader, descriptor, stage)`. Its synthetic
1.5.97/1.6.1170/1.7.99 test passed: scopes remained isolated, shader changes
were classified correctly, and corrupt bytecode, unmanifested binaries, and an
invalid-session marker were rejected. Worst-case dump paths under the actual
Skyrim installation measured 212 characters for 1.5.97, 214 for 1.6.1170, and
212 for 1.7.99 against the 259-character legacy Windows limit. The scoped
runtime-storage self-test, compile/link gate, and `git diff --check` passed; the
later current-tree PR artifact is recorded in the package preflight section.

A structurally valid manifest can still be incomplete if the game exits before
all vanilla shaders load. Reaching the main menu and comparing session key
counts remain required completeness gates.

## Offline vanilla shader archive inventory

Both versioned `Skyrim - Shaders.bsa` files were extracted read-only outside the
repository. Each contains one `shadersfx/shaders011.fxp` container. A purpose-
built parser consumed every counted group and record to exact EOF and validated
16,034 DXBC records in 1.6.1170 versus 16,044 in 1.7.99. Matching by the stable
semantic key `(group, stage, descriptor)` gives 9,054 byte-identical records,
6,972 changed records, eight removed records, and 18 added records.

All 13,054 version-scoped unique non-unchanged DXBC artifacts (6,525 from
1.6 and 6,529 from 1.7) were decompiled with `cmd_Decompiler` 1.3.2 without
a failure. They represent 13,048 globally unique hashes; six hashes occur in
both version sets. The
canonicalized HLSL differs for every changed semantic pair. The exact corrected
group distribution is:

| FXP group | Proven family | Changed | Added | Removed |
| ---: | --- | ---: | ---: | ---: |
| 2 | Grass/RunGrass | 18 VS + 18 PS | 0 | 0 |
| 5 | Effect | 0 | 0 | 1 VS + 1 PS |
| 6 | Lighting | 6,924 PS | 0 | 0 |
| 7 | Utility | 0 | 18 PS | 6 PS |
| 15, 17, 18, 22, 23, 24, 70, 71, 72, 73, 114, 115 | Image space | 12 PS | 0 | 0 |
| 8 | Water | 0 | 0 | 0 |

This corrects an early, invalid dense-header-order interpretation: group 2 is
Grass/RunGrass, not Water; group 5 is Effect; group 6 is Lighting; and group 8
Water is byte-identical. No Water constant-buffer or shader-interface change is
claimed from this archive comparison.

The changed families are contract changes, not compiler-only churn:

- In Grass/RunGrass, 1.7 moves the authoritative per-geometry lighting/fade
  inputs into the pixel-stage `b2` contract. The 1.7 pixel shaders consume
  `c16.w`, `c19.xyz`, `c20.xyz`, and `c21.w`; the associated VS/PS interface
  changes in all 18 descriptor pairs. The common shader contract is the native
  1.7 layout. The implementation selects a native adapter on pre-1.7 paths that
  binds the already-filled 0x160-byte VS per-geometry buffer directly as PS
  `b2`, without a staging global; 1.7 is not remapped. The shader catalog and
  Grass buffer contract are proven for Steam AE 1.6.1170; independent Steam SE
  1.5.97, GOG, and VR shader/runtime validation remains pending.
  `ShaderCache` reserves the native Grass pixel-constant indices 0 through 13,
  places Community Shaders additions at 14 through 16, and adds native
  `RenderDepthStencil = 7` alongside the existing `RenderDepth = 8`.
- Effect removes descriptor `0x04000800` (motion-vector normals plus multiply
  blend without vertex colour) from both stages. No safe semantic replacement
  was proven. No synthetic remap clears a blend bit or forces vertex colour:
  the legacy superset remains compilable, while the native 1.7 catalog simply
  omits this key.
- Every Lighting PS change has the same MRT rule. When `SSRParams.z > 1e-5`,
  1.7 writes colour to `o2` and `(1, 0, 0, 1)` to `o1`; otherwise `o2` receives
  the normal auxiliary value and `o1` receives the motion candidate. The CPU
  `SSRParams` upload is equivalent between 1.6 and 1.7, so this is a common-HLSL
  forward port and needs no 1.7 native translation.
- Utility's 1.7 selectors `0`, `1`, `2`, `4`, and `8` are authoritative and
  pass through literally. Only the legacy selector `3` maps to canonical `4`;
  legacy bit 20 retains `GRAYSCALE_MASK`, whereas the 1.7 shadow-mask bit 20 is
  part of the selector. Shared HLSL treats `4` and `8` as PCF. Every directional
  selector path subtracts the selected `AlphaTestRef.y` or `.z` depth bias, and
  the adjacent-cascade path subtracts `.z`, matching 1.7 disassembly. An
  independent descriptor/assembly verifier passes all 24 cases, including bias
  and sample assertions; its manifest SHA-256 is
  `2F1287E9C498FC390761A7CB4E9AC5FD3F1CFBC652E129D3BD09572C973B179A`.
  Across six descriptor families, the
  legacy descriptor-field fragment `0x6` and native fragments `0x8`/`0x10`
  produce byte-identical shaders; these fragments are not the decoded selector
  values.

The 12 changed image-space groups were mapped independently from executable
constructors and loader identities; dense enum order is not used:

| Group | Enum | Engine class |
| ---: | --- | --- |
| 15 | `ISDoubleVision` | `BSImagespaceShaderDoubleVision` |
| 17 | `ISDepthOfField` | `BSImagespaceShaderDepthOfField` |
| 18 | `ISDistantBlur` | `BSImagespaceShaderDistantBlur` |
| 22 | `ISRadialBlur` | `BSImagespaceShaderRadialBlur` |
| 23 | `ISRadialBlurMedium` | `BSImagespaceShaderRadialBlurMedium` |
| 24 | `ISRadialBlurHigh` | `BSImagespaceShaderRadialBlurHigh` |
| 70 | `ISDepthOfFieldFogged` | `BSImagespaceShaderDepthOfFieldFogged` |
| 71 | `ISDepthOfFieldMaskedFogged` | `BSImagespaceShaderDepthOfFieldMaskedFogged` |
| 72 | `ISDistantBlurFogged` | `BSImagespaceShaderDistantBlurFogged` |
| 73 | `ISDistantBlurMaskedFogged` | `BSImagespaceShaderDistantBlurMaskedFogged` |
| 114 | `ISMinify` | `BSImagespaceShaderISMinify` |
| 115 | `ISMinifyContrast` | `BSImagespaceShaderISMinifyContrast` |

`ShaderCache` maps exactly these 12 image-space descriptors: it retains the
three existing Depth of Field entries and enables the nine previously disabled
Distant Blur, Double Vision, Minify, and Radial Blur entries. The single shared
source set can therefore compile them without enabling a dense enum range or an
unproven neighboring image-space family.

The exact CPU writers are also closed. `ImageSpaceEffectGetHit::UpdateParams`
(ID 108553, RVAs `0x1516350` / `0x1582F60`) changes DoubleVision `b2:c0.x`
from zero to literal `1.0`. `ImageSpaceEffectDepthOfField::UpdateParams`
(ID 107388, `0x14E4D80` / `0x1551090`) writes the effect's `unk88` flag to
DOF/Distant `b2:c0.z`, and `ImageSpaceEffectRadialBlur::UpdateParams`
(ID 107752, `0x14F9A00` / `0x1565D30`) writes the same flag to Radial
`b2:c1.w`. The first RVA in each pair is 1.6.1170 and the second is 1.7.99.

Legacy `ImageSpaceEffectDepthOfField::Render` does not propagate the parent
flag to all six DOF/Distant children. The legacy-only adapter therefore wraps
that parent vtable slot and carries its value through a stack-safe thread-local
scope while the synchronous child renders run. Each child wrapper temporarily
writes and then restores only its selector float; it does not mutate persistent
child state. Radial uses its already-propagated child flag, and DoubleVision
uses the independently proven literal. Native 1.7 installs neither parent nor
child vtable hooks.

All 12 shared HLSL variants compile under FXC `/O3` and match the raw 1.7
constant-buffer, sampler/resource, and input/output declarations. The Minify
and MinifyContrast 3-by-3 path averages four corner red-channel samples and
explicitly zeroes Y/Z/W; it is not a four-channel average. Their compiled DXBC
hashes are respectively
`6E934861DAA9321F52BB4DE16DBCAA373AC36BD802278D56A39BC82774B9DFC6`
(3,716 bytes) and
`8C9E8F7C3EC284D3F5DDF944D65B7A4B48FCC6B15F2B7B1876939E83534F1F37`
(4,284 bytes). The complete 12-shader verification record has SHA-256
`D797B80D740E62815BE49A01779C168B28028928970AA339BD59D8A2B13F8BD4`.

The compact repository reference at
`docs/development/reference/skyrim-1.7.99` contains every one of the 6,998
changed/added/removed keys, 506 deduplicated resource/cbuffer/signature
contracts, and 56 representative contract-transition raw decompiler
outputs. It contains no raw DXBC. `PROVENANCE.md` identifies the retained HLSL
as Bethesda/cmd_Decompiler-derived reference material outside the MIT grant in
`package/Shaders/LICENSE`; it is a non-shipped development reference and must
not enter mod archives. `tools/build_shader_delta_manifest.py check` verified
26,108 content-addressed private DXBC/HLSL inputs and reproduced 6,998 keys,
506 contracts, and zero silent omissions.

## Same-fork Ghidra import validation

The Ghidra type generator was parameterized to parse the exact CommonLibSSE-NG
fork commit `0bd0be90e` and the same DirectXTK/OpenVR headers as the plugin build.
Community Shaders' production `ALL` preset selects the SE+AE+VR
`SKYRIM_CROSS_VR` wrapper model, so the audit imported both that compile-time
model and the exclusive SE/AE flat declaration models into isolated Ghidra
programs for Skyrim 1.5.97, 1.6.1170, and 1.7.99. All six transactions committed,
saved, and passed post-reopen verification with zero import errors or layout
mismatches. The complete audit has SHA-256
`3EDE29522AEEA8A14425FEE31D18DC2D47392B4FF657E47B4A98C953A467F958`.

One narrow importer correction is recorded explicitly rather than waived. The
Clang exclusive-SE primary-vtable model places `Actor::KillDying` one slot too
late; the real 1.5.97, 1.6.1170, and 1.7.99 binaries, PDB, and Address Libraries
all place `KillDying` at `0xAA` and the distinct `Resurrect` at `0xAB`. The
binary-anchor evidence has SHA-256
`DA897CB6602E5A929D43260AA2470C3E87760472271BC5C87B27A26C7A2D2B5F`.
No other declaration or vtable correction was required, and neither the
automation project nor the user's interactive Ghidra project was modified.

## Relocation and hook-site audit

The relocation inventory was regenerated from the frozen source after adding
the legacy CPU module. It records 174 source occurrences across 124 unique
runtime-ID tuples; its SHA-256 is
`122C297BBCC590DED2CF6ABB5096418DB47BAA3889A8D18966791EEC42C6ECF7`.

The companion interior-offset inventory contains 68 source occurrences. The
normalized audit classified 47 as unchanged and 12 as moved; four instruction
ambiguities and five non-instruction/data-interior sites were passed to the
Ghidra/direct-semantic reconciliation. The source inventory, normalized audit,
and Ghidra ambiguous-resolution records have SHA-256 values
`BA19D223469821C8DC105D355F06412E0162E1A96F2FE3AEC0ABA953E2058E56`,
`75D470B503FB092A3F8E90BEB9A4FB8778ECB06A3626E7C865FAC3E05C32770E`, and
`9746EDB9CE21F1C7BA66BC532BDEC21A63443D2997C37BC140B880181DC0DC96`.
The aggregate closes all 68 occurrences:
47 unchanged, 12 moved, four Ghidra-resolved, four typed-data layouts, and one
SE-only runtime-gated site, with zero unresolved. Its SHA-256 is
`CAAF6CE25B86B0E00A22FDE23078AD5E0DE9D005B8EA728619624D9836F97868`.

The 12 moved interior sites use the central
`VersionedRelocation::Select` helper, and the missing water function uses
`VersionedRelocation::ResolveID`. Instruction alignment, call-target Address
Library IDs, and Ghidra decompilation establish the following 1.7.99 changes:

| Source | Anchor | 1.6.1170 | 1.7.99 |
| --- | ---: | ---: | ---: |
| `Deferred.h` (`RenderShadowMaps`) | 36559 | `0x2EC` | `0x30A` |
| `Deferred.h` (`RenderWorld`) | 36559 | `0x841` | `0x85E` |
| `Deferred.h` (`RenderFirstPersonView`) | 36559 | `0x954` | `0x971` |
| `ShadowmapCascadeCullingFix.cpp` | 108496 | `0x1C02` | `0x1C12` |
| `GrassCollision.h` | 36564 | `0xC26` | `0xC38` |
| `InteriorSun.cpp` | 108496 | `0xE6C` | `0xE9C` |
| `Skylighting.cpp` | 36559 | `0x3A1` | `0x3BF` |
| `TerrainBlending.h` | 36559 | `0x395` | `0x3B3` |
| `Upscaling.cpp` (jitter) | 77245 | `0xE2` | `0x133` |
| `Upscaling.cpp` (precipitation) | 36559 | `0x3A1` | `0x3BF` |
| `Hooks.cpp` (grass constructor) | 15383 | `0x4F5` | `0x4FD` |
| `Hooks.cpp` (constant point lights) | 107300 | `0xB0E` | `0xB30` |

The forward CPU module adds entry/call-site hooks only for old flat desktop
runtimes:

| Contract | SE ID | AE ID | Installation rule |
| --- | ---: | ---: | --- |
| AlphaBlend render call | 100950 | 107732 | exact `call` target preflight |
| Native jitter helper | 75709 | 77518 | exact Steam SE/AE entry preflight |
| `State::SetCameraData` | 75694 | 77503 | exact Steam SE/AE entry preflight |
| FullScreenBlur `Setup`/`Render`/`UpdateParams` | 101564/101565/101566 | 108562/108563/108564 | exact vtable targets plus transactional stage-count preflight |
| `ShadowSceneNode` constructor | 99686 | 106320 | exact Steam SE/AE prologue preflight |

The three other initially ambiguous sites are unchanged in 1.7.99:
`Renderer::Init`'s `RegisterClassA` call remains at ID 77226 plus `0x15C`, and
the two volumetric-lighting patches remain at ID 107023 plus `0x406` and
`0x4A9`. Ghidra resolves the same import/instructions at each new site.

One live AE Address Library ID disappeared. The 1.6.1170 terrain water-mesh
function at ID 31846/RVA `0x5110B0` corresponds to the 1.7.99 function at RVA
`0x518D40`. Function order between preserved neighboring IDs, the unchanged
two-argument role/signature, and full decompilation distinguish it from the two
bracketing functions; reverse lookup in the format-5 database maps `0x518D40`
uniquely to ID 523588. The implementation is not byte-identical: 1.7 adds a
`BSReadWriteLock` read-side critical section around the terrain-cell map lookup.
The lock is the standalone global at Address Library ID 564236 / RVA
`0x31DF4D0`, not a `TES` member. A raw instruction scan found 66 exact code
xrefs spanning the new shared reader/writer domain; every mapped 1.6 counterpart
lacks this global, and ID 564236 has no applicable legacy mapping. Unified
Water's replacement follows a code policy of resolving that global only on AE
1.7.99 or later; the binary evidence in this report covers exactly 1.7.99. It
holds its read guard solely around `gridCells->GetCell`, releasing it before
the returned cell is dereferenced, exactly matching the native scope. SE,
AE 1.6, and VR retain their native unlocked behavior rather than using an
ineffective private lock. The complete read-only trace has SHA-256
`5BC8B8DEFDEBE8B6AE8A83D2C4572C1B0CDC44B7809A5B487F95A9D0DB54F65E`.

The shared `stl::detour_thunk` helper now accepts a resolved `uintptr_t` target,
with the existing `REL::RelocationID` overload delegating to it. This permits
the Unified Water detour to select ID 31846 or 523588 at runtime without
duplicating Detours transaction logic.

The frozen source contains 323 vtable-hook occurrences
spanning 294 unique `(vtable array, element, slot)` pairs
and 151 vtable symbols. Every pair was read from the SE,
1.6, and 1.7 programs. The legacy image-space set now includes the DOF parent
context plus the three transactionally installed `FullScreenBlur` slots
(`Setup`, `Render`, and `UpdateParams`). Exact target preflight is required
before the blur stage-count or vtable transaction performs any write. No slot
moved from 1.6 to 1.7. Two target IDs change. `BSParticleShader` slot 6 moves
from ID 108347 to 527787; its canonical instructions, mnemonics, and p-code are
identical at a 1.000000 ratio. `ImageSpaceEffectFullScreenBlur` slot 1 moves
from ID 108563 to 527788 and decompiles to the same owner-vtable `Render` role
and prototype in all three desktop programs. The 12 older SE-to-1.6 target
outliers were decompiled in both programs and retain the same generated vtable
role and prototype. The corresponding machine-readable captures are SHA-256
pinned as
`vtable-hook-inventory.json`
`C6FF4A29059F90AF422E85C06144C72492088A177A7F71D06A70B557EC67828F`,
`ghidra-ambiguous-resolution.json`
`9746EDB9CE21F1C7BA66BC532BDEC21A63443D2997C37BC140B880181DC0DC96`,
`ghidra-unifiedwater-31846-resolution.json`
`CF6E467F77CEB1FD065E819FC45E6B6EB2D9D5B66AEE50078FE3E26B8ECD8BE1`,
`ghidra-vtable-target-similarity.json`
`6C946E43DBD9AB8ABB09AEA9E7CCC87C52E1A81EFCDD8E9D6D8FC1AFE7920B1E`,
`ghidra-vtable-audit.json`
`DB1130E188D3744E0292CB140FC13CB5B1FD866998FF42A9DDCB3944836098F4`,
`ghidra-vtable-audit-final.json`
`8130166BF10BA2FCBA990F0C988262DB1ACB0F2837B66D60B5551221F1726134`,
and `ghidra-vtable-outlier-decompiles.json`
`2188E82AEE988DBC0CCE074D20BAB3AD8765F463F39855B82CE9E6BFB2246732`.
The combined frozen relocation/interior/vtable gate verdict has SHA-256
`18BCC0B94112D01243FE03095FA91D4A5C97A2E6B3C5DE7D46F2805624E0162C`.

## Renderer, upscaling, and dynamic-resolution audit

The final audit is not limited to Community Shaders' existing hooks or a short
list of named functions. A read-only export covered all 120,080 and 122,257
`.pdata` function entries in the 1.6.1170 and 1.7.99 executables. RTTI, vtable,
named graphics roots, and a bounded direct-call/reverse-caller closure produced
1,690 paired graphics roots. Of those, 1,592 are matcher-perfect. The remaining
98 roots expanded to 177 semantic pairs; 78 changed pairs belong to 29 roots
after shared-callee and ownership reconciliation. The reverse pass examined
32,105 paired callers; no normalized pair was truncated. The independent
vtable sweep paired all 639 tables and all 6,031 slots with no table-length or
slot movement.

The changed set was then classified by owner, prototype, decompilation, call
edges, and data side effects. Resource-manager teardown, `NiSkinPartition`,
`NiPSysRadialFieldModifier`, `BSCullingProcess`, particle-system management,
bone-map code, transform/geometry helpers, and matrix helpers were closed as
owner-resolved non-port changes or behaviorally equivalent implementations.
The real forward contracts are implemented in the shared HLSL/legacy shader
adapters described above and in the legacy CPU module below. No actionable
Steam static gap remains in this bounded surface; runtime image/behavior QA is
still required. The exhaustive
catalog, paired comparison, and changed-pair projection have SHA-256 values
`580E362E3E40F2D7CD31796BBC6DB5A6661F3647A3AB800CA14AB9C4721C184A`,
`1868A40ECBBDE201ECA928E9FC52B2AA8507730EB55278B09F8368BB494C6827`,
and `8C0F5693AAF1CAB216FD012F7B6614326886D5010C01E68C87F02D1A6A072819`.
The final narrative and machine-readable closure records have SHA-256 values
`C7CA67E5FF48E8744C085D5CB991B3A06B361E0BF6DC301AAF6487EBEC50FB35`
and `644DC1A009C77315DA1FF836B125531646D6F7E3F186E17D2BE18BB68E0DADFA`.

The upscaling/dynamic-resolution functions that remained exact are enumerated
here:

| Entry point | AL ID | 1.6.1170 RVA | 1.7.99 RVA | Result |
| --- | ---: | ---: | ---: | --- |
| `ConsoleFunc::handler::CheckRenderTargets` | 22874 | `0x36B500` | `0x371B70` | exact |
| `ConsoleFunc::handler::FunctionDisplayRenderTarget` | 23106 | `0x375630` | `0x37BCA0` | exact |
| `ConsoleFunc::handler::DynamicResolution` | 23220 | `0x37A4E0` | `0x380EE0` | exact |
| `Renderer::CreateSwapChain` | 77242 | `0xE44360` | `0x10098A0` | exact |
| `Renderer::SaveRenderTargetToFile` | 77316 | `0xE48ED0` | `0x100E470` | exact |
| DXGI-factory caller | 77396 | `0xE4C830` | `0x1011E00` | exact |
| `BSImagespaceShaderISUpsampleDynamicResolution::~...` | 106135 | `0x149D370` | `0x1508FB0` | exact |
| `BSImagespaceShaderISUpsampleDynamicResolution::Func13` | 106259 | `0x149EE20` | `0x150AA60` | exact |
| `BSImagespaceShader::Func9` | 107731 | `0x14F8870` | `0x1564B90` | exact |
| `ReloadShaders` | 108324 | `0x150BFF0` | `0x1578BE0` | exact |
| `D3D11CreateDeviceAndSwapChain` import | 110097 | `0x154CBC2` | `0x15B91B2` | exact import stub |
| `CreateDXGIFactory` import | 110109 | `0x154CC22` | `0x15B9212` | exact import stub |

The apparent 1.7-only graphics/upscaling anchors are data registrations rather
than new executable implementations:

| 1.7-only anchor | `.rdata` RVA | Pointer/data RVA | Xref classification |
| --- | ---: | ---: | --- |
| `ToggleDLSS` | `0x184C8B0` | `0x2095B50` | static console-command record beside `tdlss` and its help text; no function xref |
| `bSynchronizeDynamicWidthAndHeight:Display` | `0x1A74910` | `0x20D04E8` | display-setting registration; no function xref |
| `NintendoSwitch2` | `0x1AE5428` | none | platform/session telemetry string; no pointer or function xref |
| `NINTENDOSWITCH2` | `0x1AE54A0` | none | platform/session telemetry string; no pointer or function xref |
| `uShadowFilterType:Display` | `0x1B30090` | `0x20D6B70` | display-setting registration used to explain the Utility catalog; no standalone renderer function xref |

The direct Ghidra xref results were cross-checked with nearby-record and
RIP-relative executable scans. Three additional 1.7-only named data symbols,
`gIni_bEnableAutoDynamicResolution_Display` (AL 380759/RVA `0x20B5870`),
`gIniPref_iVSyncPresentInterval_Display` (AL 388999/RVA `0x20D03A0`), and
`gIni_bAssertOnShaderCompileAtRuntime_Display` (AL 389014/RVA `0x20D0418`),
have no independent old function body to transplant. Their consumers were
nevertheless included in the exhaustive closure; the renderer changes below
separate active forward contracts from audited dormant code, rather than
drawing conclusions from symbol names alone.

### Forward CPU graphics contracts

`LegacyGraphicsCompatibility` installs only on flat desktop runtimes older than
1.7.99. Native 1.7.99 executes its own code with zero hooks, descriptor remaps,
or vtable writes from this module. Every legacy hook has an exact binary
preflight; the multi-write FullScreenBlur operation is transactional and rolls
back on failure. The frozen reconciliation examined 13 legacy candidates: four
were invariant, and the nine changed candidates collapse into the four active
adapter families below.

| 1.7 behavior | Legacy implementation |
| --- | --- |
| AlphaBlend supplies viewport `{x, y, width, height}` rather than `{left, top, right, bottom}`. | The verified legacy call-site thunk subtracts left/top from right/bottom before calling the unchanged viewport helper. |
| The native jitter helper preserves vanilla projection scale separately, and `State::SetCameraData` bit 9 consumes that preserved pair. | A core detour captures `State+0x44/+0x48` immediately after the native legacy jitter helper. A renderer-thread `thread_local` snapshot is keyed and validity-checked by `State*`; the bit-9 `SetCameraData` wrapper substitutes it only for the synchronous native call, then restores the live values. This works whether the optional Upscaling feature is installed or absent. |
| FullScreenBlur owns 11 stages, configures `ISCopyDynamicFetchDisabled` in slot 10, propagates child selector flags, selects the fractional or dynamic-fetch-disabled copy, and sets slot-8 `c0.x` to 1. | Exact setup/shutdown/destructor opcode preflights change all three immediate counts from 10 to 11 as one transaction. Setup configures slot 10; Render scopes/restores child flags and the slot 9/10 swap; UpdateParams writes the selector. Vector `size()`, child availability, and conflicting aliases are checked before use. |
| `ShadowSceneNode` construction durably zero-initializes the new graphics tail. | The legacy constructor wrapper zeroes exactly `+0x2CC..+0x2E3` after native construction; the following lighting/camera tail at `+0x2E8` remains native-owned. |

`Renderer::Begin` does contain an additional 1.7 client/render-extent reset
clause, but it is not an active Steam 1.7.99 contract. The clause requires the
new byte at RVA `0x20CFBF0` to be zero; the audited executable initializes it to
one. An exhaustive executable-section scan found exactly two RIP-relative
references, both reads (AL 524254 at RVA `0x1008940` and `Renderer::Begin` at
RVA `0x1009BAD`), with no writer, address-taker, or absolute pointer anywhere
in the image. AL 524254 uses the same gate to suppress fullscreen size-change
requests. The dormant/platform-gated extent branch is therefore documented but
not transplanted into older Steam runtimes. The gate trace has SHA-256
`0507CC0AFC00D8304A1D3D6F4D0D80F23398BCBDAB1C6D6E3C609DD197C93CC0`.

The camera adapter is deliberately independent of `Upscaling::PostPostLoad`.
The optional feature still hooks its versioned `Renderer::Begin` call site
(`0xE5` on SE, `0xE2` on Steam AE 1.6, and `0x133` on the source branch shared
by GOG/1.7) to configure Community Shaders upscaling after the native helper.
Only the Steam 1.7.99 site is statically proven here; GOG remains a separate
validation target. Because the core detours the helper entry (SE/AE IDs
75709/77518), that later call naturally records the vanilla result first. The
same core adapter wraps `State::SetCameraData`
(SE/AE IDs 75694/77503), so bit 9 never depends on an optional feature's
lifecycle. The direct caller scan found 15 callers in each executable; only
the 1.7 AL 52727 path supplies bit 9, which is why the old-only wrapper acts
only on that flag and otherwise passes through.

The active CPU-side backports and their entry preflights are statically proven
against the Steam 1.5.97 and Steam 1.6.1170 executables. The refreshed
three-runtime proof covers all 13 candidates, including `State::UpdateJitter`,
`State::SetCameraData`, AlphaBlend, FullScreenBlur, ShadowSceneNode, and the
invariant `SetViewport`/`Configure*` candidates. Its SHA-256 is
`CA51E7389D1127E4F71112710EAB4F6D5D66E95A34FE829C573ADD1B33440E1D`;
the complete outside-graphics closure records above include the final lifecycle
and source hashes. The frozen
`LegacyGraphicsCompatibility.cpp` SHA-256 is
`D24EF14D3F84701A94FAD816B664F4A637596B62BFF487BC3A23E3EDC194450E`.
This is narrower than end-to-end runtime proof. GOG was not in the binary
corpus: an unexpected GOG opcode/target shape is rejected at the individual
adapter preflight. VR was also outside the proof corpus and the entire CPU
module intentionally installs zero hooks or writes there. The existing shared
legacy shader adapters are compiled for VR, but VR shader/runtime validation
remains pending.

No named `Present` implementation or import exists in either Ghidra program;
the game invokes presentation through a DXGI COM vtable. Present-time behavior
therefore remains a runtime-capture gate rather than a statically proven
equivalence. Static proof also does not replace deterministic scene comparison
or actual SKSE-hosted initialization on each claimed runtime.

## Build, PE, and loader preflight

After the canonical shader-type migration, a multi-runtime `Dev-Fast`
historical checkpoint compiled and linked successfully. That checkpoint DLL is
33,134,080 bytes with SHA-256
`C62ACDB1EEB1F880804D85C2F64A8180148473497FC98B5312602B6BE24D799A`;
the PDB SHA-256 is
`28379398CDB7FD14759837DC6400AAB0A57C2D5AF9C2AC6CC21B33079A1D751D`.

An external native verifier maps the DLL as a PE image, normally initializes
all 68 direct imported DLLs, and inspects the exported SKSE metadata. It passes
with all three `SKSEPlugin_Load`, `SKSEPlugin_Query`, and `SKSEPlugin_Version`
exports present; plugin version `1.8.0.0`; `versionIndependenceEx = 1` (no
loader-imposed pre/post-1.6.629 structure restriction); `versionIndependence =
1` (Address Library);
zero exact-version list; and zero global SKSE minimum, as required for a single
SE/AE/VR DLL. This is a dependency/export preflight only. Community Shaders has
static `BSFixedString` initializers that call relocated Skyrim functions during
CRT initialization, so only an actual SKSE-hosted run can validly prove the DLL
itself completes initialization and loads.

After the outside-graphics source freeze, the incremental `Dev-Fast` build and
an immediate no-work rerun both passed. That non-shipping DLL has SHA-256
`5C78849DDE555FD34A200A9C7604C556894AC63E56A49D3A6B459D0D62210F0C`.

## Package namespace and PR-build preflight

Required third-party notices remain beside their source assets, but package
staging now relocates nine generic `LICENSE`/`README` files under
`SKSE/Plugins/CommunityShaders/Notices` or `Documentation`. This avoids false
mod-manager conflicts in shared `Data/Shaders`, `Data/Renderdoc`, and
`Data/textures` paths without dropping attribution.

The dirty CI-parity PR build completed successfully. Its AIO archive contains
exactly 343 source payload entries plus DLL/PDB, and its Core archive contains
273 source entries plus DLL/PDB. Both have zero forbidden generic documents,
zero path collisions, and exact hashes for every relocated notice. The AIO
archive SHA-256 is
`D384D443B4AD6A9F54E4504D9B601C619B44D916AE3E7D40BD48C7AFA2EC801C`;
the Core archive SHA-256 is
`AFC1AE3AB1201AA119A0621F1DE2E9351A8103B64C86B0D564F75D09D916AEDE`.
Both embed DLL SHA-256
`A32B7EFADA5B18B99A6B1003746FAE998CF7802197D12502A69FBCE909A73CFB`.
The DLL passes PE, import, export, and SKSE metadata validation, but it is not a
shipping artifact because LTO is disabled and its build description is dirty.

## Reverse-engineering environments

- The existing Bethesda Ghidra project remains the automation/reference
  project.
- A separate project copy was created in an isolated audit workspace and
  opened for interactive inspection. It has its own project database and lock,
  so it cannot mutate the automation project.
- The interactive program is
  `skyrim/ae1.7/SkyrimSE.exe.unpacked.exe`.

## Runtime-test rules

- Steam must remain closed.
- Launch only a verified Steamless executable.
- Prefer `coc Riverwood` over save loading.
- Force a deterministic weather such as `fw 81a` and hold time, camera,
  resolution, INI settings, plugin set, and feature settings constant.
- Capture logs, shader dumps, cache metadata, screenshots, and crash artifacts
  separately for every runtime.

## Pending evidence

- Obtain independently sourced 1.5.97 shader assets and validate the shared
  shader catalog/contract offline. The only local candidate is a mixed-version
  backup and is not accepted as provenance. Steam 1.5.97 CPU hook patterns are
  statically proven; that does not substitute for the missing shader corpus or
  runtime test.
- Validate GOG separately. It was not part of the executable or shader corpus;
  legacy CPU hooks reject unexpected opcode/target shapes, but no GOG runtime
  compatibility result is claimed here.
- Validate the legacy adapters against a Skyrim VR binary/shader corpus. The
  universal build and CommonLib VR compile gate pass, but this audit does not
  contain independent VR runtime assets. The new CPU graphics module performs
  no VR hooks or writes.
- Capture the DXGI COM `Present` path at runtime; it has no named static import
  or implementation to compare in the current Ghidra programs.
- Runtime hook-site validation for every relocation with a version-specific
  instruction offset.
- Actual SKSE-hosted DLL initialization, a complete-main-menu shader manifest,
  and deterministic visual/cache comparison across 1.5.97, 1.6.1170, and
  1.7.99.

The `Dev-Fast` compile/link gate passes on the frozen source after the Unified
Water lock, shared shader adapters, corrected `State` layout, and four legacy
CPU graphics contract groups. The final clean shipping rebuild is still
pending; earlier DLL/PDB hashes in this report are historical validation
checkpoints, not final shipping hashes.
