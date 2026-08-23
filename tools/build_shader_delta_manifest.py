#!/usr/bin/env python3
"""Build and verify the compact Skyrim 1.6.1170 -> 1.7.99 shader delta.

The checked-in output contains hashes, reflected contracts, and selected
decompiler-HLSL reference samples, but no raw DXBC or full decompiler corpus. A
deep check accepts the private, content-addressed extraction tree and verifies
every referenced DXBC/HLSL file.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path


SCHEMA = 1
EXPECTED_COUNTS = {"changed": 6972, "added": 18, "removed": 8}
EXPECTED_ARCHIVE_HASHES = {
    "1.6": "37735579E3537C32815E192EB58D89F90D6D4AC876EB56C9A7310DA6F5FDE47A",
    "1.7": "EEFCEA0E47A1C19F81DFB0EA7F310ACA0D0BDF4675CFF8C99F5C312DD8A36AF7",
}
FIXED_GROUPS = {
    0: "BloodSplatter",
    1: "DistantTree",
    2: "Grass",
    3: "Particle",
    4: "Sky",
    5: "Effect",
    6: "Lighting",
    7: "Utility",
    8: "Water",
}
PROVEN_IMAGE_GROUPS = {
    15: ("ISDoubleVision", "BSImagespaceShaderDoubleVision"),
    17: ("ISDepthOfField", "BSImagespaceShaderDepthOfField"),
    18: ("ISDistantBlur", "BSImagespaceShaderDistantBlur"),
    22: ("ISRadialBlur", "BSImagespaceShaderRadialBlur"),
    23: ("ISRadialBlurMedium", "BSImagespaceShaderRadialBlurMedium"),
    24: ("ISRadialBlurHigh", "BSImagespaceShaderRadialBlurHigh"),
    70: ("ISDepthOfFieldFogged", "BSImagespaceShaderDepthOfFieldFogged"),
    71: ("ISDepthOfFieldMaskedFogged", "BSImagespaceShaderDepthOfFieldMaskedFogged"),
    72: ("ISDistantBlurFogged", "BSImagespaceShaderDistantBlurFogged"),
    73: ("ISDistantBlurMaskedFogged", "BSImagespaceShaderDistantBlurMaskedFogged"),
    114: ("ISMinify", "BSImagespaceShaderISMinify"),
    115: ("ISMinifyContrast", "BSImagespaceShaderISMinifyContrast"),
}
CANONICAL_IMAGE_SOURCES = {
    15: "package/Shaders/ISDoubleVision.hlsl",
    17: "package/Shaders/ISDepthOfField.hlsl",
    18: "package/Shaders/ISDepthOfField.hlsl",
    22: "package/Shaders/ISRadialBlur.hlsl",
    23: "package/Shaders/ISRadialBlur.hlsl",
    24: "package/Shaders/ISRadialBlur.hlsl",
    70: "package/Shaders/ISDepthOfField.hlsl",
    71: "package/Shaders/ISDepthOfField.hlsl",
    72: "package/Shaders/ISDepthOfField.hlsl",
    73: "package/Shaders/ISDepthOfField.hlsl",
    114: "package/Shaders/ISSAOMinify.hlsl",
    115: "package/Shaders/ISSAOMinify.hlsl",
}
CSV_FIELDS = [
    "key",
    "status",
    "group_index",
    "family",
    "imagespace_enum",
    "imagespace_class",
    "stage",
    "descriptor_hex",
    "dxbc_sha256_1_6",
    "raw_hlsl_sha256_1_6",
    "contract_sha256_1_6",
    "dxbc_sha256_1_7",
    "raw_hlsl_sha256_1_7",
    "contract_sha256_1_7",
    "canonical_source",
    "source_disposition",
]
RAW_REFERENCE_FIELDS = [
    "file",
    "source_version",
    "group_index",
    "family",
    "stage",
    "semantic_keys",
    "descriptors",
    "dxbc_sha256",
    "raw_hlsl_sha256",
    "selection_reason",
]


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def canonical_json_bytes(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def split_manifest(value: str) -> list[str]:
    return [] if not value else [item.strip() for item in value.split(" | ") if item.strip()]


def effective_resources(value: str) -> list[str]:
    # These are injected by cmd_Decompiler/3Dmigoto, not reflected game resources.
    return [
        item
        for item in split_manifest(value)
        if "IniParams" not in item and "StereoParams" not in item
    ]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as stream:
        return list(csv.DictReader(stream))


def validate_identity(rows: list[dict[str, str]]) -> None:
    for index, expected in FIXED_GROUPS.items():
        names = {row["group_name"] for row in rows if int(row["group_index"]) == index}
        if names != {expected}:
            raise RuntimeError(f"group {index}: expected {expected!r}, found {sorted(names)!r}")

    changed_image_rows = [
        row for row in rows if row["status"] == "changed" and int(row["group_index"]) >= 9
    ]
    found_groups = {int(row["group_index"]) for row in changed_image_rows}
    if found_groups != set(PROVEN_IMAGE_GROUPS) or len(changed_image_rows) != 12:
        raise RuntimeError(
            f"changed image-space identity mismatch: groups={sorted(found_groups)}, "
            f"rows={len(changed_image_rows)}"
        )
    for row in changed_image_rows:
        index = int(row["group_index"])
        expected_enum, expected_class = PROVEN_IMAGE_GROUPS[index]
        if (row["imagespace_enum"], row["imagespace_class"]) != (expected_enum, expected_class):
            raise RuntimeError(
                f"image-space group {index}: expected {(expected_enum, expected_class)!r}, "
                f"found {(row['imagespace_enum'], row['imagespace_class'])!r}"
            )


def canonical_source(row: dict[str, str]) -> str:
    group = int(row["group_index"])
    if group == 2:
        return "package/Shaders/RunGrass.hlsl"
    if group == 5:
        return "package/Shaders/Effect.hlsl"
    if group == 6:
        return "package/Shaders/Lighting.hlsl"
    if group == 7:
        return "package/Shaders/Utility.hlsl"
    return CANONICAL_IMAGE_SOURCES.get(group, "")


def source_disposition(row: dict[str, str]) -> str:
    group = int(row["group_index"])
    status = row["status"]
    if group == 2:
        return (
            "common-1.7-source; implementation selects a pre-1.7 direct-bind of "
            "the already-filled VS PerGeometry buffer as PS b2"
        )
    if group == 5:
        return (
            "legacy superset remains compilable; no synthetic descriptor remap; "
            "native 1.7 catalog omits this key"
        )
    if group == 6:
        return "common authoritative-1.7 MRT source; no native adapter"
    if group == 7:
        if status == "removed":
            return "legacy catalog evidence; translate only legacy selector 3 to canonical 4"
        return "common source; 1.7 literal; translate only legacy selector 3 to canonical 4"
    if group == 15:
        return "common authoritative-1.7 source; legacy DoubleVision selector adapter writes literal 1; native 1.7 untouched"
    if group in {17, 18, 70, 71, 72, 73}:
        return "common authoritative-1.7 source; legacy DOF parent context supplies unk88 to child selector adapter; native 1.7 untouched"
    if group in {22, 23, 24}:
        return "common authoritative-1.7 source; legacy Radial selector adapter mirrors child unk88; native 1.7 untouched"
    if group in {114, 115}:
        return "common authoritative-1.7 source; no native adapter"
    return "reference evidence only; no source port identified"


def make_contract(row: dict[str, str], suffix: str) -> dict[str, object] | None:
    if not row[f"dxbc_sha256_{suffix}"]:
        return None
    return {
        "shader_model": row[f"shader_model_{suffix}"] or None,
        "resources": effective_resources(row[f"resource_bindings_{suffix}"]),
        "constant_buffers": split_manifest(row[f"constant_buffers_{suffix}"]),
        "input_output_signature": row[f"input_output_{suffix}"] or None,
    }


def verify_artifact(
    archive_root: Path,
    status_by_key: dict[tuple[str, str], dict[str, str]],
    version: str,
    dxbc_hash: str,
    file_hash_cache: dict[Path, str],
) -> str:
    if not dxbc_hash:
        return ""
    key = (version, dxbc_hash.upper())
    status = status_by_key.get(key)
    if status is None or status.get("hlsl_exists", "").lower() != "true":
        raise RuntimeError(f"missing successful decompiler record for {version} {dxbc_hash}")
    dxbc = archive_root / "decompiled" / version / f"{dxbc_hash}.dxbc"
    hlsl = archive_root / "decompiled" / version / f"{dxbc_hash}.hlsl"
    for path in (dxbc, hlsl):
        if not path.is_file():
            raise RuntimeError(f"missing referenced artifact: {path}")
        if path not in file_hash_cache:
            file_hash_cache[path] = sha256_file(path)
    if file_hash_cache[dxbc] != dxbc_hash.upper():
        raise RuntimeError(f"DXBC hash mismatch: {dxbc}")
    raw_hlsl_hash = status["hlsl_sha256"].upper()
    if file_hash_cache[hlsl] != raw_hlsl_hash:
        raise RuntimeError(f"raw HLSL hash mismatch: {hlsl}")
    return raw_hlsl_hash


def build_raw_references(
    archive_root: Path,
    delta_rows: list[dict[str, str]],
    status_by_key: dict[tuple[str, str], dict[str, str]],
) -> tuple[dict[str, bytes], bytes]:
    """Select small comparison/evidence samples without vendoring raw DXBC."""

    selected: dict[tuple[str, str], dict[str, object]] = {}

    def add(row: dict[str, str], suffix: str, version: str, reason: str) -> None:
        dxbc_hash = row[f"dxbc_sha256_{suffix}"].upper()
        if not dxbc_hash:
            return
        status = status_by_key.get((version, dxbc_hash))
        if status is None or status.get("hlsl_exists", "").lower() != "true":
            raise RuntimeError(f"raw-reference decompiler record missing: {version} {dxbc_hash}")
        entry = selected.setdefault(
            (version, dxbc_hash),
            {
                "versions": {version},
                "groups": {int(row["group_index"])},
                "families": {row["group_name"]},
                "stages": {row["stage"]},
                "semantic_keys": set(),
                "descriptors": set(),
                "reasons": set(),
                "raw_hlsl_sha256": status["hlsl_sha256"].upper(),
            },
        )
        entry["semantic_keys"].add(row["key"])
        entry["descriptors"].add(row["descriptor_hex"])
        entry["reasons"].add(reason)

    for row in delta_rows:
        group = int(row["group_index"])
        if group == 2:
            # All unique old/new bodies make the VS->PS contract move reviewable.
            add(row, "1_6", "1.6", "RunGrass legacy comparison algorithm")
            add(row, "1_7", "1.7", "RunGrass authoritative algorithm")
        elif group == 5 and row["status"] == "removed":
            add(row, "1_6", "1.6", "removed Effect 0x04000800 evidence; no safe remap")
        elif group == 7 and row["status"] == "added":
            add(row, "1_7", "1.7", "added Utility catalog-key evidence")
        elif group == 7 and row["status"] == "removed":
            add(row, "1_6", "1.6", "legacy Utility selector comparison algorithm")
        elif group in PROVEN_IMAGE_GROUPS:
            # Preserve both sides of every independently mapped image-space
            # transition implemented by the common forward-contract sources.
            add(row, "1_6", "1.6", "legacy image-space forward-port comparison algorithm")
            add(row, "1_7", "1.7", "authoritative implemented image-space algorithm")

    # One pair is sufficient for Lighting: the source-level MRT change is uniform
    # across all 6,924 permutations and every key/hash remains in shader-delta.csv.
    lighting = next(
        row
        for row in delta_rows
        if int(row["group_index"]) == 6 and row["stage"] == "PS"
    )
    add(lighting, "1_6", "1.6", "representative Lighting MRT comparison")
    add(lighting, "1_7", "1.7", "representative authoritative Lighting MRT algorithm")

    raw_outputs: dict[str, bytes] = {}
    manifest_rows: list[dict[str, str]] = []
    for (version, dxbc_hash), entry in sorted(selected.items()):
        groups = sorted(entry["groups"])
        families = sorted(entry["families"])
        stages = sorted(entry["stages"])
        source = archive_root / "decompiled" / version / f"{dxbc_hash}.hlsl"
        data = source.read_bytes()
        actual_hlsl_hash = sha256_bytes(data)
        if actual_hlsl_hash != entry["raw_hlsl_sha256"]:
            raise RuntimeError(f"selected raw HLSL hash mismatch: {source}")
        group_label = "-".join(f"g{group:03d}" for group in groups)
        family_label = re.sub(r"[^A-Za-z0-9_.-]+", "-", "-".join(families)).strip("-")
        stage_label = "-".join(stages)
        relative = (
            f"raw-reference/{group_label}-{family_label}-{stage_label}-{version}-"
            f"{dxbc_hash[:16]}.hlsl"
        )
        if relative in raw_outputs and raw_outputs[relative] != data:
            raise RuntimeError(f"raw-reference filename collision: {relative}")
        raw_outputs[relative] = data
        manifest_rows.append(
            {
                "file": relative,
                "source_version": version,
                "group_index": ";".join(str(value) for value in groups),
                "family": ";".join(families),
                "stage": ";".join(stages),
                "semantic_keys": ";".join(sorted(entry["semantic_keys"])),
                "descriptors": ";".join(sorted(entry["descriptors"])),
                "dxbc_sha256": dxbc_hash,
                "raw_hlsl_sha256": actual_hlsl_hash,
                "selection_reason": "; ".join(sorted(entry["reasons"])),
            }
        )

    from io import StringIO

    manifest_stream = StringIO(newline="")
    writer = csv.DictWriter(
        manifest_stream, fieldnames=RAW_REFERENCE_FIELDS, lineterminator="\n"
    )
    writer.writeheader()
    writer.writerows(manifest_rows)
    return raw_outputs, manifest_stream.getvalue().encode("utf-8")


def build_payloads(archive_root: Path, script_path: Path) -> dict[str, bytes]:
    manifests = archive_root / "manifests"
    comparison_path = manifests / "shader-comparison.csv"
    status_path = manifests / "decompiler-status.csv"
    summary_path = manifests / "summary.json"
    group_summary_path = manifests / "group-summary.csv"
    rows = read_csv(comparison_path)
    validate_identity(rows)
    delta_rows = [row for row in rows if row["status"] != "unchanged"]
    counts = Counter(row["status"] for row in delta_rows)
    if dict(counts) != EXPECTED_COUNTS or len(delta_rows) != 6998:
        raise RuntimeError(f"delta completeness mismatch: counts={dict(counts)}, rows={len(delta_rows)}")
    keys = [row["key"] for row in delta_rows]
    if len(set(keys)) != len(keys):
        raise RuntimeError("duplicate semantic keys in delta")

    status_rows = read_csv(status_path)
    status_by_key = {
        (row["version"], row["dxbc_sha256"].upper()): row for row in status_rows
    }
    file_hash_cache: dict[Path, str] = {}
    contracts: dict[str, dict[str, object]] = {}
    compact_rows: list[dict[str, str]] = []
    for row in delta_rows:
        output: dict[str, str] = {
            "key": row["key"],
            "status": row["status"],
            "group_index": row["group_index"],
            "family": row["group_name"],
            "imagespace_enum": row["imagespace_enum"],
            "imagespace_class": row["imagespace_class"],
            "stage": row["stage"],
            "descriptor_hex": row["descriptor_hex"],
            "canonical_source": canonical_source(row),
            "source_disposition": source_disposition(row),
        }
        for suffix, version in (("1_6", "1.6"), ("1_7", "1.7")):
            dxbc_hash = row[f"dxbc_sha256_{suffix}"].upper()
            output[f"dxbc_sha256_{suffix}"] = dxbc_hash
            output[f"raw_hlsl_sha256_{suffix}"] = verify_artifact(
                archive_root, status_by_key, version, dxbc_hash, file_hash_cache
            )
            contract = make_contract(row, suffix)
            if contract is None:
                output[f"contract_sha256_{suffix}"] = ""
            else:
                contract_hash = sha256_bytes(canonical_json_bytes(contract))
                existing = contracts.setdefault(contract_hash, contract)
                if existing != contract:
                    raise RuntimeError(f"contract hash collision: {contract_hash}")
                output[f"contract_sha256_{suffix}"] = contract_hash
        compact_rows.append(output)

    # Preserve semantic archive order, which is itself deterministic.
    from io import StringIO

    csv_stream = StringIO(newline="")
    writer = csv.DictWriter(csv_stream, fieldnames=CSV_FIELDS, lineterminator="\n")
    writer.writeheader()
    writer.writerows(compact_rows)
    delta_csv = csv_stream.getvalue().encode("utf-8")
    contracts_json = json_bytes(
        {
            "schema": SCHEMA,
            "note": (
                "Resources exclude cmd_Decompiler/3Dmigoto's injected IniParams(t120) and "
                "StereoParams(t125); constant buffers and signatures preserve decompiler text."
            ),
            "contracts": [
                {"sha256": contract_hash, **contracts[contract_hash]}
                for contract_hash in sorted(contracts)
            ],
        }
    )
    raw_outputs, raw_reference_manifest = build_raw_references(
        archive_root, delta_rows, status_by_key
    )
    provenance = (
        "# Skyrim 1.7.99 shader-delta provenance\n\n"
        "This directory is a non-shipped development reference. The manifest records hashes and "
        "reflected contracts derived from Bethesda Game Studios' Skyrim Special Edition shader "
        "archives for runtimes 1.6.1170 and 1.7.99. It contains a deliberately small set of "
        "representative contract-transition cmd_Decompiler HLSL outputs, but no raw DXBC.\n\n"
        "The raw DXBC and decompiler output remain Bethesda/Skyrim-derived reference material. "
        "No MIT license or other copyright grant is asserted for that material, and "
        "`package/Shaders/LICENSE` does not apply to it. Keep raw artifacts outside shipped mod "
        "folders and outside distributable archives.\n\n"
        "Regenerate or deep-verify this compact manifest only from lawfully obtained local game "
        "files. `shader-delta.csv` identifies every changed, added, and removed semantic key; "
        "`shader-contracts.json` content-addresses resources, constant buffers, and signatures. "
        "`raw-reference-manifest.csv` proves the source hashes and reason for every retained sample.\n"
    ).encode("utf-8")
    readme = (
        "# Skyrim 1.7.99 shader delta\n\n"
        "This compact development package covers exactly 6,998 non-identical semantic keys from "
        "`shaders011.fxp`: 6,972 changed, 18 added, and 8 removed. The semantic key is FXP group, "
        "stage, and descriptor. `manifest.json` pins both source archives and all generator inputs.\n\n"
        "- `shader-delta.csv`: complete corrected family/stage/descriptor and 1.6/1.7 DXBC, raw-HLSL, "
        "contract, canonical-source, and disposition hashes.\n"
        "- `shader-contracts.json`: deduplicated effective resources, cbuffers, and I/O signatures.\n"
        "- `raw-reference/`: only unique RunGrass transition algorithms, the removed Effect pair, "
        "added/removed Utility catalog-key evidence, one representative Lighting MRT pair, and both sides of "
        "the 12 independently identified image-space changes.\n"
        "- `raw-reference-manifest.csv`: provenance and content hashes for those samples.\n\n"
        "The 1.7 catalog/CPU contract is authoritative and passes through literally. Native "
        "translation is legacy-only. The shader catalog and shader-side CPU contracts compared "
        "here are proven for Steam AE 1.6.1170 and Steam AE 1.7.99; independent Steam SE "
        "1.5.97, GOG, and VR shader/runtime validation remains pending. Grass directly binds "
        "the already-populated "
        "legacy VS PerGeometry buffer as PS b2 when the implementation selects a pre-1.7 path; "
        "it does not use a staging global. Utility "
        "maps only legacy selector 3 to canonical 4. Effect 0x04000800 receives no synthetic "
        "remap: the legacy superset remains compilable and the native 1.7 catalog omits it. "
        "Image-space adapters are also legacy-only: DoubleVision writes selector 1, a scoped "
        "DOF parent context supplies `unk88` to the six DOF/Distant children, the three Radial "
        "classes use their already-propagated child `unk88`, and the two "
        "Minify classes require no CPU shim. Native 1.7 receives no adapter or vtable write.\n\n"
        "Run `tools/build_shader_delta_manifest.py check --archive-root <private-analysis-root> "
        "--output-root <this-directory>` for a deep, byte-for-byte verification. See "
        "`PROVENANCE.md` before redistributing any raw-reference file.\n"
    ).encode("utf-8")

    outputs = {
        "README.md": readme,
        "shader-delta.csv": delta_csv,
        "shader-contracts.json": contracts_json,
        "raw-reference-manifest.csv": raw_reference_manifest,
        "PROVENANCE.md": provenance,
        **raw_outputs,
    }
    summary = json.loads(summary_path.read_text(encoding="utf-8-sig"))
    observed_archives = {
        "1.6": summary["Archive16"]["Sha256"].upper(),
        "1.7": summary["Archive17"]["Sha256"].upper(),
    }
    if observed_archives != EXPECTED_ARCHIVE_HASHES:
        raise RuntimeError(f"source archive identity mismatch: {observed_archives}")
    metadata = {
        "schema": SCHEMA,
        "semantic_key": "(FXP group index, shader stage, descriptor)",
        "source_runtimes": {"old": "1.6.1170", "authoritative": "1.7.99"},
        "source_archive_sha256": observed_archives,
        "source_manifest_sha256": {
            "summary.json": sha256_file(summary_path),
            "group-summary.csv": sha256_file(group_summary_path),
            "shader-comparison.csv": sha256_file(comparison_path),
            "decompiler-status.csv": sha256_file(status_path),
        },
        "generator_sha256": sha256_file(script_path),
        "counts": {
            **EXPECTED_COUNTS,
            "non_unchanged": len(compact_rows),
            "unique_semantic_keys": len(set(keys)),
            "silent_omissions": 0,
            "contract_records": len(contracts),
            "content_addressed_dxbc_hlsl_files_verified": len(file_hash_cache),
            "retained_raw_hlsl_samples": len(raw_outputs),
        },
        "group_identity": {
            "fixed_fxp_groups": {str(key): value for key, value in FIXED_GROUPS.items()},
            "proven_changed_image_space_groups": {
                str(key): {"enum": value[0], "class": value[1]}
                for key, value in PROVEN_IMAGE_GROUPS.items()
            },
            "warning": (
                "Do not infer image-space family names from dense header enum order; only the "
                "12 mappings above are independently proven for changed groups."
            ),
        },
        "forward_contract": {
            "rule": "1.7 descriptors and CPU contracts pass through literally",
            "validation_scope": (
                "The shader catalog and shader-side CPU contracts are proven for Steam AE "
                "1.6.1170 and Steam AE 1.7.99; independent Steam SE 1.5.97, GOG, and VR "
                "shader/runtime validation remains pending."
            ),
            "legacy_adapters": [
                (
                    "Grass: the implementation selects a pre-1.7 direct-bind of the "
                    "already-populated 0x160-byte VS PerGeometry buffer as PS b2"
                ),
                "Utility: translate legacy shadow-filter selector 3 to canonical selector 4 only",
                "Image-space DoubleVision: write selector 1 on legacy runtimes only",
                (
                    "Image-space DOF/Distant: carry the legacy parent unk88 through a scoped "
                    "render context; Radial uses its already-propagated child unk88; native "
                    "1.7 installs no adapter or vtable write"
                ),
            ],
            "effect_0x04000800": (
                "no synthetic remap; legacy superset remains compilable; "
                "native 1.7 catalog omits the key"
            ),
        },
        "output_sha256": {name: sha256_bytes(data) for name, data in outputs.items()},
    }
    outputs["manifest.json"] = json_bytes(metadata)
    return outputs


def write_outputs(output_root: Path, outputs: dict[str, bytes]) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    for name, data in outputs.items():
        path = output_root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def validate_written(output_root: Path, expected: dict[str, bytes]) -> None:
    actual_names = {
        path.relative_to(output_root).as_posix()
        for path in output_root.rglob("*")
        if path.is_file()
    }
    expected_names = set(expected)
    if actual_names != expected_names:
        raise RuntimeError(
            f"output file-set mismatch; missing={sorted(expected_names - actual_names)}, "
            f"extra={sorted(actual_names - expected_names)}"
        )
    for name, data in expected.items():
        path = output_root / name
        if not path.is_file():
            raise RuntimeError(f"missing generated output: {path}")
        actual = path.read_bytes()
        if actual != data:
            raise RuntimeError(
                f"non-deterministic or stale output: {path}; "
                f"expected {sha256_bytes(data)}, found {sha256_bytes(actual)}"
            )

    rows = read_csv(output_root / "shader-delta.csv")
    counts = Counter(row["status"] for row in rows)
    if dict(counts) != EXPECTED_COUNTS or len({row["key"] for row in rows}) != 6998:
        raise RuntimeError("written delta failed count/key validation")
    contracts_payload = json.loads((output_root / "shader-contracts.json").read_text("utf-8"))
    contracts = {item["sha256"] for item in contracts_payload["contracts"]}
    for row in rows:
        for field in ("contract_sha256_1_6", "contract_sha256_1_7"):
            if row[field] and row[field] not in contracts:
                raise RuntimeError(f"dangling contract reference {row[field]} in {row['key']}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("build", "check"))
    parser.add_argument("--archive-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    script_path = Path(__file__).resolve()
    expected = build_payloads(args.archive_root.resolve(), script_path)
    if args.command == "build":
        write_outputs(args.output_root.resolve(), expected)
    validate_written(args.output_root.resolve(), expected)
    metadata = json.loads(expected["manifest.json"])
    print(
        f"PASS {args.command}: {metadata['counts']['non_unchanged']} keys, "
        f"{metadata['counts']['contract_records']} contracts, "
        f"{metadata['counts']['silent_omissions']} omissions"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # concise CI-facing failure
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
