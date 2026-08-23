#!/usr/bin/env python3
"""Validate and compare runtime-scoped Community Shaders shader storage.

This proves storage identity and manifest integrity only; it does not prove
that a shader contract or plugin build is compatible with a game runtime.
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import tempfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any, Iterable


SCHEMA_VERSION = 2
EXECUTABLE_PATH_ID_HEX_LENGTH = 24
DUMP_SESSION_ID_HEX_LENGTH = 24
LOADER_READABLE_PREFIX_LENGTH = 12
LOADER_PATH_ID_HEX_LENGTH = 24
BYTECODE_PATH_ID_HEX_LENGTH = 24
SESSION_PREFIX = "s-"
MAX_LEGACY_PATH_CHARS = 259
HEX_DIGITS = frozenset("0123456789abcdef")
REQUIRED_SHADER_FIELDS = {
    "schema_version",
    "runtime_version",
    "executable_sha256",
    "dump_session",
    "source",
    "loader",
    "descriptor",
    "descriptor_hex",
    "stage",
    "bytecode_sha256",
    "bytecode_size",
    "path",
}


class ValidationFailure(RuntimeError):
    pass


@dataclass
class ManifestSummary:
    path: Path
    runtime_version: str
    executable_sha256: str
    dump_session: str
    shader_count: int = 0
    hashes_by_key: dict[tuple[str, int, str], set[str]] = field(default_factory=dict)


@dataclass
class Comparison:
    unchanged: set[tuple[str, int, str]]
    changed: set[tuple[str, int, str]]
    only_left: set[tuple[str, int, str]]
    only_right: set[tuple[str, int, str]]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sanitize_path_component(component: str) -> str:
    result = "".join(
        character
        if (character.isascii() and (character.isalnum() or character in "-_."))
        else "_"
        for character in component
    )
    return result if result not in {"", ".", ".."} else "unknown"


def _is_lower_hex(value: str, length: int) -> bool:
    return len(value) == length and all(character in HEX_DIGITS for character in value)


def executable_directory(executable_identity: str, location: str) -> str:
    unavailable_prefix = "unavailable-"
    if executable_identity.startswith(unavailable_prefix):
        fallback_identity = executable_identity.removeprefix(unavailable_prefix)
        if not _is_lower_hex(fallback_identity, 32):
            raise ValidationFailure(f"{location}: unavailable executable identity is malformed")
        return f"u-{fallback_identity[:EXECUTABLE_PATH_ID_HEX_LENGTH]}"
    if not _is_lower_hex(executable_identity, 64):
        raise ValidationFailure(f"{location}: executable_sha256 is not a full SHA-256 digest")
    return executable_identity[:EXECUTABLE_PATH_ID_HEX_LENGTH]


def loader_directory(loader: str) -> str:
    readable = sanitize_path_component(loader)[:LOADER_READABLE_PREFIX_LENGTH]
    identity = hashlib.sha256(loader.encode("utf-8")).hexdigest()[:LOADER_PATH_ID_HEX_LENGTH]
    return f"{readable}--{identity}"


def validate_session_id(session: str, location: str) -> None:
    if not session.startswith(SESSION_PREFIX) or not _is_lower_hex(
        session.removeprefix(SESSION_PREFIX), DUMP_SESSION_ID_HEX_LENGTH
    ):
        raise ValidationFailure(f"{location}: dump session ID is malformed")


def _require_string(record: dict[str, Any], field_name: str, location: str) -> str:
    value = record[field_name]
    if not isinstance(value, str) or not value:
        raise ValidationFailure(f"{location}: {field_name} must be a non-empty string")
    return value


def _safe_manifest_target(session_root: Path, relative_value: str, location: str) -> Path:
    relative_path = PurePosixPath(relative_value)
    if relative_path.is_absolute() or ".." in relative_path.parts:
        raise ValidationFailure(f"{location}: unsafe dump path {relative_value!r}")

    root = session_root.resolve()
    target = root.joinpath(*relative_path.parts).resolve()
    if root != target and root not in target.parents:
        raise ValidationFailure(f"{location}: dump path escapes its session root")
    return target


def validate_manifest(path: Path) -> ManifestSummary:
    path = path.resolve()
    if path.name != "manifest.jsonl" or len(path.parents) < 4 or path.parents[3].name != "ShaderDump":
        raise ValidationFailure(f"{path}: expected a ShaderDump runtime/session manifest")

    session_root = path.parent
    expected_session = session_root.name
    expected_executable = session_root.parent.name
    expected_runtime = session_root.parent.parent.name
    summary = ManifestSummary(path, expected_runtime, "", expected_session)

    if not path.is_file():
        raise ValidationFailure(f"{path}: manifest does not exist")
    invalid_marker = session_root / "manifest.invalid"
    if invalid_marker.exists():
        raise ValidationFailure(f"{invalid_marker}: dump session was marked invalid")
    validate_session_id(expected_session, str(path))

    seen_dump_paths: dict[str, tuple[str, int, str, str]] = {}
    manifested_files: set[Path] = set()
    with path.open("r", encoding="utf-8") as stream:
        for line_number, raw_line in enumerate(stream, 1):
            if not raw_line.strip():
                continue
            location = f"{path}:{line_number}"
            try:
                record = json.loads(raw_line)
            except json.JSONDecodeError as error:
                raise ValidationFailure(f"{location}: invalid JSON: {error}") from error
            if not isinstance(record, dict):
                raise ValidationFailure(f"{location}: record is not a JSON object")

            missing = REQUIRED_SHADER_FIELDS.difference(record)
            if missing:
                raise ValidationFailure(f"{location}: missing fields: {', '.join(sorted(missing))}")
            if record["schema_version"] != SCHEMA_VERSION:
                raise ValidationFailure(
                    f"{location}: schema {record['schema_version']!r}, expected {SCHEMA_VERSION}"
                )

            runtime = _require_string(record, "runtime_version", location)
            executable = _require_string(record, "executable_sha256", location)
            session = _require_string(record, "dump_session", location)
            source = _require_string(record, "source", location)
            loader = _require_string(record, "loader", location)
            stage = _require_string(record, "stage", location)
            bytecode_hash = _require_string(record, "bytecode_sha256", location)
            relative_path = _require_string(record, "path", location)

            if sanitize_path_component(runtime) != expected_runtime:
                raise ValidationFailure(f"{location}: runtime does not match directory {expected_runtime!r}")
            if executable_directory(executable, location) != expected_executable:
                raise ValidationFailure(f"{location}: executable identity does not match its directory")
            if summary.executable_sha256 and executable != summary.executable_sha256:
                raise ValidationFailure(f"{location}: manifest mixes full executable identities")
            summary.executable_sha256 = executable
            if session != expected_session:
                raise ValidationFailure(f"{location}: session does not match directory {expected_session!r}")
            if source != "BSShader::LoadShaders":
                raise ValidationFailure(f"{location}: source is not a vanilla BSShader loader")
            if stage not in {"vs", "ps"}:
                raise ValidationFailure(f"{location}: unsupported vanilla shader stage {stage!r}")
            if not _is_lower_hex(bytecode_hash, 64):
                raise ValidationFailure(f"{location}: bytecode_sha256 is not a SHA-256 digest")
            if (
                not isinstance(record["descriptor"], int)
                or record["descriptor"] < 0
                or record["descriptor"] > 0xFFFFFFFF
            ):
                raise ValidationFailure(f"{location}: descriptor must be an unsigned 32-bit integer")
            descriptor = record["descriptor"]
            if record["descriptor_hex"] != f"{descriptor:X}":
                raise ValidationFailure(f"{location}: descriptor_hex does not match descriptor")
            if not isinstance(record["bytecode_size"], int) or record["bytecode_size"] <= 0:
                raise ValidationFailure(f"{location}: bytecode_size must be positive")

            target = _safe_manifest_target(session_root, relative_path, location)
            if not target.is_file():
                raise ValidationFailure(f"{location}: dump file is missing: {target}")
            if target.stat().st_size != record["bytecode_size"]:
                raise ValidationFailure(f"{location}: dump size does not match bytecode_size")
            if sha256_file(target) != bytecode_hash:
                raise ValidationFailure(f"{location}: dump SHA-256 does not match manifest")
            expected_filename = (
                f"{descriptor:X}.{stage}.{bytecode_hash[:BYTECODE_PATH_ID_HEX_LENGTH]}.bin"
            )
            if target.name != expected_filename:
                raise ValidationFailure(
                    f"{location}: filename {target.name!r}, expected {expected_filename!r}"
                )
            if PurePosixPath(relative_path).parent.as_posix() != loader_directory(loader):
                raise ValidationFailure(f"{location}: loader directory is not collision-safe or does not match loader")

            identity = (loader, descriptor, stage, bytecode_hash)
            prior_identity = seen_dump_paths.setdefault(relative_path, identity)
            if prior_identity != identity:
                raise ValidationFailure(f"{location}: one dump path describes multiple shaders")

            summary.shader_count += 1
            manifested_files.add(target)
            summary.hashes_by_key.setdefault((loader, descriptor, stage), set()).add(bytecode_hash)

    if summary.shader_count == 0:
        raise ValidationFailure(f"{path}: manifest contains no shader records")
    unmanifested_files = {
        candidate.resolve() for candidate in session_root.rglob("*.bin")
    }.difference(manifested_files)
    if unmanifested_files:
        first_unmanifested = min(unmanifested_files, key=str)
        raise ValidationFailure(f"{path}: unmanifested shader dump exists: {first_unmanifested}")
    return summary


def validate_cache_info(path: Path) -> None:
    path = path.resolve()
    if path.name.lower() != "info.ini" or len(path.parents) < 3 or path.parents[2].name != "ShaderCache":
        raise ValidationFailure(f"{path}: expected runtime-scoped ShaderCache Info.ini")

    expected_executable = path.parent.name
    expected_runtime = path.parent.parent.name
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    try:
        with path.open("r", encoding="utf-8-sig") as stream:
            parser.read_file(stream)
    except (OSError, configparser.Error) as error:
        raise ValidationFailure(f"{path}: invalid INI: {error}") from error

    if not parser.has_section("Cache"):
        raise ValidationFailure(f"{path}: missing [Cache] section")
    cache = parser["Cache"]
    if cache.get("StorageSchemaVersion") != str(SCHEMA_VERSION):
        raise ValidationFailure(f"{path}: cache storage schema is not {SCHEMA_VERSION}")
    runtime = cache.get("RuntimeVersion", "")
    if sanitize_path_component(runtime) != expected_runtime:
        raise ValidationFailure(f"{path}: full runtime metadata does not match its directory")
    executable = cache.get("ExecutableSHA256", "")
    if executable_directory(executable, str(path)) != expected_executable:
        raise ValidationFailure(f"{path}: full executable metadata does not match its directory")


def compare_manifests(left: ManifestSummary, right: ManifestSummary) -> Comparison:
    left_keys = set(left.hashes_by_key)
    right_keys = set(right.hashes_by_key)
    shared = left_keys & right_keys
    return Comparison(
        unchanged={key for key in shared if left.hashes_by_key[key] == right.hashes_by_key[key]},
        changed={key for key in shared if left.hashes_by_key[key] != right.hashes_by_key[key]},
        only_left=left_keys - right_keys,
        only_right=right_keys - left_keys,
    )


def _format_key(key: tuple[str, int, str]) -> str:
    loader, descriptor, stage = key
    return f"{loader} {descriptor:X}.{stage}"


def _find_storage(data_dir: Path) -> tuple[list[Path], list[Path]]:
    dump_sessions = sorted(path for path in data_dir.glob("ShaderDump/*/*/*") if path.is_dir())
    cache_infos = sorted(data_dir.glob("ShaderCache/*/*/Info.ini"))
    return dump_sessions, cache_infos


def command_validate(data_dir: Path) -> int:
    dump_sessions, cache_infos = _find_storage(data_dir)
    failures: list[str] = []
    for session_root in dump_sessions:
        manifest = session_root / "manifest.jsonl"
        try:
            summary = validate_manifest(manifest)
            print(
                f"OK dump {summary.runtime_version} {summary.dump_session}: "
                f"{summary.shader_count} records, {len(summary.hashes_by_key)} shader keys"
            )
        except ValidationFailure as error:
            failures.append(str(error))
    for cache_info in cache_infos:
        try:
            validate_cache_info(cache_info)
            print(f"OK cache {cache_info.parent.parent.name}/{cache_info.parent.name}")
        except ValidationFailure as error:
            failures.append(str(error))

    if not dump_sessions and not cache_infos:
        failures.append(f"{data_dir}: no runtime-scoped shader dumps or caches found")
    legacy_info = data_dir / "ShaderCache" / "Info.ini"
    if legacy_info.exists():
        print(f"IGNORED legacy unscoped cache metadata: {legacy_info}")
    for failure in failures:
        print(f"ERROR {failure}")
    return 1 if failures else 0


def command_compare(left_path: Path, right_path: Path, details: bool) -> int:
    try:
        left = validate_manifest(left_path)
        right = validate_manifest(right_path)
    except ValidationFailure as error:
        print(f"ERROR {error}")
        return 1

    comparison = compare_manifests(left, right)
    print(f"left:  {left.runtime_version} {left.dump_session} ({len(left.hashes_by_key)} keys)")
    print(f"right: {right.runtime_version} {right.dump_session} ({len(right.hashes_by_key)} keys)")
    print(f"unchanged:  {len(comparison.unchanged)}")
    print(f"changed:    {len(comparison.changed)}")
    print(f"left only:  {len(comparison.only_left)}")
    print(f"right only: {len(comparison.only_right)}")

    if details:
        for label, keys in (
            ("CHANGED", comparison.changed),
            ("LEFT_ONLY", comparison.only_left),
            ("RIGHT_ONLY", comparison.only_right),
        ):
            for key in sorted(keys):
                print(f"{label} {_format_key(key)}")
    return 0


def build_worst_case_dump_path(game_root: Path, runtime: str) -> Path:
    executable_path_id = "e" * EXECUTABLE_PATH_ID_HEX_LENGTH
    session = SESSION_PREFIX + "a" * DUMP_SESSION_ID_HEX_LENGTH
    loader = "L" * LOADER_READABLE_PREFIX_LENGTH + "--" + "d" * LOADER_PATH_ID_HEX_LENGTH
    filename = (
        "FFFFFFFF.ps."
        + "b" * BYTECODE_PATH_ID_HEX_LENGTH
        + ".bin"
    )
    return game_root / "Data" / "ShaderDump" / runtime / executable_path_id / session / loader / filename


def command_path_length(game_root: Path) -> int:
    failed = False
    for runtime in ("1.5.97.0", "1.6.1170.0", "1.7.99.0"):
        path = build_worst_case_dump_path(game_root, runtime)
        path_length = len(str(path))
        status = "OK" if path_length <= MAX_LEGACY_PATH_CHARS else "ERROR"
        print(f"{status} {runtime}: {path_length}/{MAX_LEGACY_PATH_CHARS} chars: {path}")
        failed |= path_length > MAX_LEGACY_PATH_CHARS
    return 1 if failed else 0


def _write_fixture_session(
    data_dir: Path,
    runtime: str,
    executable_hash: str,
    session: str,
    payloads: Iterable[tuple[str, int, str, bytes]],
) -> Path:
    session_root = data_dir / "ShaderDump" / runtime / executable_directory(executable_hash, "fixture") / session
    records: list[dict[str, Any]] = []
    for loader, descriptor, stage, payload in payloads:
        bytecode_hash = hashlib.sha256(payload).hexdigest()
        relative = Path(loader_directory(loader)) / (
            f"{descriptor:X}.{stage}.{bytecode_hash[:BYTECODE_PATH_ID_HEX_LENGTH]}.bin"
        )
        target = session_root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        records.append(
            {
                "schema_version": SCHEMA_VERSION,
                "runtime_version": runtime,
                "executable_sha256": executable_hash,
                "dump_session": session,
                "source": "BSShader::LoadShaders",
                "loader": loader,
                "descriptor": descriptor,
                "descriptor_hex": f"{descriptor:X}",
                "stage": stage,
                "bytecode_sha256": bytecode_hash,
                "bytecode_size": len(payload),
                "path": relative.as_posix(),
            }
        )
    manifest = session_root / "manifest.jsonl"
    manifest.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
    return manifest


def _write_fixture_cache(data_dir: Path, runtime: str, executable_hash: str) -> Path:
    info = data_dir / "ShaderCache" / runtime / executable_directory(executable_hash, "fixture") / "Info.ini"
    info.parent.mkdir(parents=True, exist_ok=True)
    info.write_text(
        "[Cache]\n"
        f"StorageSchemaVersion={SCHEMA_VERSION}\n"
        f"RuntimeVersion={runtime}\n"
        f"ExecutableSHA256={executable_hash}\n"
        "PluginVersion=1.0.0\n",
        encoding="utf-8",
    )
    return info


def command_self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="cs-shader-storage-") as temporary:
        data_dir = Path(temporary) / "Data"
        versions = ("1.5.97.0", "1.6.1170.0", "1.7.99.0")
        manifests: list[Path] = []
        for index, version in enumerate(versions, 1):
            executable_hash = f"{index:x}" * 64
            payloads = [
                ("Lighting", 0x1234, "vs", b"shared vertex bytecode"),
                ("Lighting", 0x1234, "ps", f"pixel bytecode {version}".encode()),
            ]
            manifests.append(
                _write_fixture_session(
                    data_dir,
                    version,
                    executable_hash,
                    f"{SESSION_PREFIX}{index:0{DUMP_SESSION_ID_HEX_LENGTH}x}",
                    payloads,
                )
            )
            validate_cache_info(_write_fixture_cache(data_dir, version, executable_hash))

        summaries = [validate_manifest(manifest) for manifest in manifests]
        assert len({summary.path.parent.parent for summary in summaries}) == 3
        comparison = compare_manifests(summaries[1], summaries[2])
        assert len(comparison.unchanged) == 1
        assert len(comparison.changed) == 1
        assert not comparison.only_left
        assert not comparison.only_right

        corrupt_target = next(manifests[2].parent.glob("*/*.bin"))
        corrupt_target.write_bytes(b"corrupt")
        try:
            validate_manifest(manifests[2])
        except ValidationFailure:
            pass
        else:
            raise AssertionError("corrupt shader bytecode was accepted")

        unmanifested_dump = manifests[1].parent / "orphan" / "orphan.bin"
        unmanifested_dump.parent.mkdir()
        unmanifested_dump.write_bytes(b"unmanifested")
        try:
            validate_manifest(manifests[1])
        except ValidationFailure:
            pass
        else:
            raise AssertionError("unmanifested shader bytecode was accepted")

        invalid_marker = manifests[0].parent / "manifest.invalid"
        invalid_marker.write_text("synthetic rollback failure\n", encoding="utf-8")
        try:
            validate_manifest(manifests[0])
        except ValidationFailure:
            pass
        else:
            raise AssertionError("invalid dump session marker was accepted")

    print(
        "synthetic storage self-test passed: three runtimes isolated, comparison classified, "
        "corruption, unmanifested bytecode, and invalid-session marker rejected"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate runtime-scoped shader cache metadata and cryptographic dump manifests."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser("validate", help="validate every cache/dump scope under a Data folder")
    validate.add_argument("--data-dir", type=Path, default=Path("Data"))

    compare = subparsers.add_parser("compare", help="compare two verified dump sessions")
    compare.add_argument("left", type=Path, help="left manifest.jsonl")
    compare.add_argument("right", type=Path, help="right manifest.jsonl")
    compare.add_argument("--details", action="store_true", help="list every changed/missing shader key")

    path_length = subparsers.add_parser(
        "path-length", help="check worst-case dump paths against the legacy Windows MAX_PATH limit"
    )
    path_length.add_argument("game_root", type=Path, help="Skyrim Special Edition installation root")

    subparsers.add_parser("self-test", help="run synthetic isolation/integrity tests")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "validate":
        return command_validate(args.data_dir)
    if args.command == "compare":
        return command_compare(args.left, args.right, args.details)
    if args.command == "path-length":
        return command_path_length(args.game_root)
    return command_self_test()


if __name__ == "__main__":
    raise SystemExit(main())
