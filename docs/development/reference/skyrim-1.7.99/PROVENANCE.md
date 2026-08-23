# Skyrim 1.7.99 shader-delta provenance

This directory is a non-shipped development reference. The manifest records hashes and reflected contracts derived from Bethesda Game Studios' Skyrim Special Edition shader archives for runtimes 1.6.1170 and 1.7.99. It contains a deliberately small set of representative contract-transition cmd_Decompiler HLSL outputs, but no raw DXBC.

The raw DXBC and decompiler output remain Bethesda/Skyrim-derived reference material. No MIT license or other copyright grant is asserted for that material, and `package/Shaders/LICENSE` does not apply to it. Keep raw artifacts outside shipped mod folders and outside distributable archives.

Regenerate or deep-verify this compact manifest only from lawfully obtained local game files. `shader-delta.csv` identifies every changed, added, and removed semantic key; `shader-contracts.json` content-addresses resources, constant buffers, and signatures. `raw-reference-manifest.csv` proves the source hashes and reason for every retained sample.
