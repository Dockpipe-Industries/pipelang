#!/usr/bin/env python3
"""Build portable standalone CLI archives; publication is a separate workflow step."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[4]
TARGETS = (("linux", "amd64"), ("linux", "arm64"), ("darwin", "amd64"), ("darwin", "arm64"), ("windows", "amd64"))
COMMANDS = ("pipelang", "pipelang-streamc", "pipelang-native-cpp")


def main():
    version = (ROOT / "VERSION").read_text().strip()
    binary = os.environ.get("DOCKPIPE_BIN", "dockpipe")
    output = Path(subprocess.check_output([binary, "scope", "artifacts", "release"], text=True).strip())
    output.mkdir(parents=True, exist_ok=True)
    records = []
    for system, architecture in TARGETS:
        name = f"pipelang_{version}_{system}_{architecture}"
        suffix = ".zip" if system == "windows" else ".tar.gz"
        archive = output / (name + suffix)
        environment = dict(os.environ, GOWORK="off", CGO_ENABLED="0", GOOS=system, GOARCH=architecture)
        with tempfile.TemporaryDirectory(prefix="pipelang-release-") as temporary:
            stage = Path(temporary)
            for command in COMMANDS:
                executable = command + (".exe" if system == "windows" else "")
                subprocess.run(["go", "build", "-buildvcs=false", "-trimpath", "-o", str(stage / executable), "./src/cmd/" + command], cwd=ROOT, env=environment, check=True)
            for filename in ("LICENSE", "README.md"):
                (stage / filename).write_bytes((ROOT / filename).read_bytes())
            if system == "windows":
                with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as stream:
                    for path in sorted(stage.iterdir()):
                        stream.write(path, path.name)
            else:
                with tarfile.open(archive, "w:gz") as stream:
                    for path in sorted(stage.iterdir()):
                        stream.add(path, arcname=path.name)
        records.append({"name": archive.name, "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(), "size_bytes": archive.stat().st_size})
    (output / "release-manifest.json").write_text(json.dumps({"schema": 1, "version": version, "artifacts": records}, indent=2) + "\n")
    (output / "SHA256SUMS.txt").write_text("".join(record["sha256"] + "  " + record["name"] + "\n" for record in records))
    print(output)


if __name__ == "__main__":
    main()
