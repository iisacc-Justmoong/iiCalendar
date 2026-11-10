#!/usr/bin/env python3
"""Prepare verified ICU resources using temporary build/ intermediates (POSIX only)."""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = "78.3"
SHA256 = "3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0"
URL = f"https://github.com/unicode-org/icu/releases/download/release-{VERSION}/icu4c-{VERSION}-sources.tgz"


def resource_paths(root):
    root = Path(root).resolve(strict=True)
    resources, build_root = root / "resources", root / "build"
    deps = resources / "dependencies"
    prefix = deps / "icu/install"
    build = build_root / "icu-bootstrap"
    for path in (resources, deps, deps / "icu", prefix, deps / "icu/source", build_root, build):
        if path.is_symlink():
            raise ValueError(f"Unsafe symbolic link in ICU path: {path}")
    if not deps.resolve().is_relative_to(resources.resolve()):
        raise ValueError("ICU resources escape the project")
    return deps, build, prefix


def installed_prefix_ready(prefix):
    header = prefix / "include/unicode/uvernum.h"
    libraries = [prefix / "lib" / f"libicu{name}.a" for name in ("uc", "i18n", "data")]
    if not header.is_file() or not all(path.is_file() for path in libraries):
        return False
    pattern = r'^\s*#\s*define\s+U_ICU_VERSION\s+"' + re.escape(VERSION) + r'"\s*$'
    return bool(re.search(pattern, header.read_text(encoding="utf-8"), re.MULTILINE))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if platform.system() not in ("Darwin", "Linux"):
        parser.error("Use an ICU development package and ICU_ROOT on this host")
    root = Path(__file__).resolve().parents[1]
    deps, build, prefix = resource_paths(root)
    if prefix.exists():
        if not installed_prefix_ready(prefix):
            raise SystemExit("Existing ICU prefix is not a complete pinned installation; preserve and inspect it")
        print(f"Reusing verified-version ICU_ROOT={prefix}")
        return
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / "icu.tgz"
    if archive.is_symlink():
        raise SystemExit("The ICU archive cannot be a symbolic link")
    if not archive.exists():
        pending = deps / "icu.tgz.download"
        if pending.exists() or pending.is_symlink():
            raise SystemExit("An ICU download already exists; preserve and inspect it")
        created = False
        try:
            with pending.open("xb") as target:
                created = True
                with urllib.request.urlopen(URL, timeout=120) as source:
                    while chunk := source.read(1024 * 1024):
                        target.write(chunk)
            os.link(pending, archive)
        finally:
            if created:
                pending.unlink(missing_ok=True)
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
    if build.exists():
        raise SystemExit("Refusing to replace an existing ICU bootstrap build directory")
    build.mkdir(parents=True, exist_ok=False)
    commands = [
        [str(source / "configure"), f"--prefix={prefix}", "--disable-shared", "--enable-static",
         "--disable-tests", "--disable-samples", "--with-pic"],
        ["make", "-j", str(args.jobs)],
        ["make", "install"],
    ]
    try:
        for name, command in zip(("configure", "compile", "install"), commands):
            print(f"ICU {VERSION}: {name}", flush=True)
            subprocess.run(command, cwd=build, check=True)
    finally:
        shutil.rmtree(build)
    print(f"ICU_ROOT={prefix}")


if __name__ == "__main__":
    main()
