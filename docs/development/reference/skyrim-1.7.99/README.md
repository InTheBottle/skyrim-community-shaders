# Skyrim 1.7.99 shader delta

This compact development package covers exactly 6,998 non-identical semantic keys from `shaders011.fxp`: 6,972 changed, 18 added, and 8 removed. The semantic key is FXP group, stage, and descriptor. `manifest.json` pins both source archives and all generator inputs.

- `shader-delta.csv`: complete corrected family/stage/descriptor and 1.6/1.7 DXBC, raw-HLSL, contract, canonical-source, and disposition hashes.
- `shader-contracts.json`: deduplicated effective resources, cbuffers, and I/O signatures.
- `raw-reference/`: only unique RunGrass transition algorithms, the removed Effect pair, added/removed Utility catalog-key evidence, one representative Lighting MRT pair, and both sides of the 12 independently identified image-space changes.
- `raw-reference-manifest.csv`: provenance and content hashes for those samples.

The 1.7 catalog/CPU contract is authoritative and passes through literally. Native translation is legacy-only. The shader catalog and shader-side CPU contracts compared here are proven for Steam AE 1.6.1170 and Steam AE 1.7.99; independent Steam SE 1.5.97, GOG, and VR shader/runtime validation remains pending. Grass directly binds the already-populated legacy VS PerGeometry buffer as PS b2 when the implementation selects a pre-1.7 path; it does not use a staging global. Utility maps only legacy selector 3 to canonical 4. Effect 0x04000800 receives no synthetic remap: the legacy superset remains compilable and the native 1.7 catalog omits it. Image-space adapters are also legacy-only: DoubleVision writes selector 1, a scoped DOF parent context supplies `unk88` to the six DOF/Distant children, the three Radial classes use their already-propagated child `unk88`, and the two Minify classes require no CPU shim. Native 1.7 receives no adapter or vtable write.

Run `tools/build_shader_delta_manifest.py check --archive-root <private-analysis-root> --output-root <this-directory>` for a deep, byte-for-byte verification. See `PROVENANCE.md` before redistributing any raw-reference file.
