#!/usr/bin/env python3
"""Build a verified ICU release inside this project's build/ tree (POSIX only)."""
import argparse
import hashlib
from pathlib import Path
import platform
import subprocess
import tarfile
import urllib.request

VERSION = "78.3"
SHA256 = "3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0"
URL = f"https://github.com/unicode-org/icu/releases/download/release-{VERSION}/icu4c-{VERSION}-sources.tgz"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if platform.system() not in ("Darwin", "Linux"):
        parser.error("Use an ICU development package and ICU_ROOT on this host")
    root = Path(__file__).resolve().parents[1]
    deps = root / "build" / "deps"
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / "icu.tgz"
    if not archive.exists():
        pending = deps / "icu.tgz.download"
        with urllib.request.urlopen(URL, timeout=120) as source, pending.open("wb") as target:
            while chunk := source.read(1024 * 1024):
                target.write(chunk)
        pending.replace(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SHA256:
        raise SystemExit(f"ICU SHA-256 mismatch: {digest}; archive was not extracted")
    source = deps / "icu" / "source"
    if not source.exists():
        with tarfile.open(archive) as tar:
            # Reject traversal, special files and unsafe links before writing anything.
            for member in tar.getmembers():
                resolved = (deps / member.name).resolve()
                if (Path(member.name).is_absolute()
                        or not resolved.is_relative_to(deps.resolve())
                        or not (member.isfile() or member.isdir())):
                    raise SystemExit("Unsafe ICU archive path")
            tar.extractall(deps)
    build = deps / "icu" / "build"
    prefix = deps / "icu" / "install"
    build.mkdir(parents=True, exist_ok=True)
    commands = [
        [str(source / "configure"), f"--prefix={prefix}", "--disable-shared", "--enable-static",
         "--disable-tests", "--disable-samples", "--with-pic"],
        ["make", "-j", str(args.jobs)],
        ["make", "install"],
    ]
    for name, command in zip(("configure", "compile", "install"), commands):
        print(f"ICU {VERSION}: {name}", flush=True)
        with (build / f"{name}.log").open("w") as log:
            subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(f"ICU_ROOT={prefix}")


if __name__ == "__main__":
    main()
