#!/usr/bin/env python3
"""Record and verify the complete installed experimental native model SDK."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import tempfile
import os

PROTOCOL = "pipelang.native-model-sdk.v1"
VERSION = "0.1.0"
MAXIMUM_MANIFEST_BYTES = 16 << 20


def exact_object(value, names, label):
    if not isinstance(value, dict) or set(value) != set(names):
        raise ValueError(label + " has an unexpected field set")


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON field: " + key)
        result[key] = value
    return result


def load_json(data):
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=unique_keys,
                          parse_constant=reject_constant)
    except RecursionError as error:
        raise ValueError("JSON nesting exceeds its capacity") from error


def reject_constant(value):
    raise ValueError("Non-JSON scalar: " + value)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(1 << 20):
            digest.update(block)
    return digest.hexdigest()


def exact_hash(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError("Expected an exact lowercase SHA256")
    return value


def relative_path(value):
    if not isinstance(value, str) or not value or "\\" in value:
        raise ValueError("SDK paths must be portable relative paths")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in (".", "..") for part in value.split("/")):
        raise ValueError("SDK path escapes its installed prefix")
    if str(path) != value:
        raise ValueError("SDK path is not canonical")
    return path


def inventory(prefix, roots, excluded):
    records = {}
    for root_name in roots:
        root = prefix / relative_path(root_name)
        if root.is_symlink() or not root.exists():
            raise ValueError("SDK root is missing or symbolic: " + root_name)
        selected = sorted(root.rglob("*")) if root.is_dir() else [root]
        for path in selected:
            if path.is_symlink():
                raise ValueError("SDK symbolic file or directory refused: " + str(path))
            if path.is_dir():
                continue
            if not path.is_file():
                raise ValueError("SDK entry is not a regular file: " + str(path))
            name = path.relative_to(prefix).as_posix()
            if name == excluded:
                continue
            records[name] = {"path": name, "sha256": file_hash(path), "bytes": path.stat().st_size}
    return [records[name] for name in sorted(records)]


def verify(prefix, manifest_path, expected_hash):
    exact_hash(expected_hash)
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise ValueError("SDK manifest must be a regular installed file")
    with manifest_path.open("rb") as source:
        data = source.read(MAXIMUM_MANIFEST_BYTES + 1)
    if len(data) > MAXIMUM_MANIFEST_BYTES or sha256(data) != expected_hash:
        raise ValueError("SDK manifest checksum or size mismatch")
    manifest = load_json(data)
    exact_object(manifest, ("protocol", "version", "roots", "files"), "SDK manifest")
    if manifest["protocol"] != PROTOCOL or manifest["version"] != VERSION:
        raise ValueError("Unexpected SDK protocol or exact version")
    roots = manifest["roots"]
    if not isinstance(roots, list) or not roots or len(roots) > 16:
        raise ValueError("Invalid SDK root inventory")
    for root in roots:
        relative_path(root)
    if len(set(roots)) != len(roots):
        raise ValueError("Duplicate SDK root")
    files = manifest["files"]
    if not isinstance(files, list) or not files or len(files) > 4096:
        raise ValueError("Invalid SDK file inventory")
    names = set()
    for record in files:
        exact_object(record, ("path", "sha256", "bytes"), "SDK file")
        relative_path(record["path"])
        exact_hash(record["sha256"])
        if type(record["bytes"]) is not int or record["bytes"] < 0 or record["path"] in names:
            raise ValueError("Invalid or duplicate SDK file identity")
        names.add(record["path"])
    excluded = manifest_path.relative_to(prefix).as_posix()
    actual = inventory(prefix, roots, excluded)
    if actual != files:
        raise ValueError("Installed SDK content differs from its pinned manifest")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--roots", nargs="+", required=True)
    args = parser.parse_args()
    prefix = args.prefix.absolute()
    manifest_path = args.manifest.absolute()
    excluded = manifest_path.relative_to(prefix).as_posix()
    files = inventory(prefix, args.roots, excluded)
    manifest = {"protocol": PROTOCOL, "version": VERSION, "roots": args.roots, "files": files}
    data = (json.dumps(manifest, sort_keys=True, indent=2, allow_nan=False) + "\n").encode()
    if len(data) > MAXIMUM_MANIFEST_BYTES:
        raise ValueError("SDK manifest exceeds its capacity")
    with tempfile.NamedTemporaryFile(dir=manifest_path.parent, prefix=".sdk-manifest-", delete=False) as staged:
        staged.write(data)
        temporary = Path(staged.name)
    try:
        os.replace(temporary, manifest_path)
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
