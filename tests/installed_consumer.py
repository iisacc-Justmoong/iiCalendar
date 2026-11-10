"""Build an installed consumer in an isolated, self-cleaning test workspace."""
import argparse
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import subprocess
import sys
import ii_file_provider as provider


def redirect_manifest(content, build, workspace):
    for name in ("install_local_manifest.txt", "${CMAKE_INSTALL_MANIFEST}"):
        old = build.resolve().as_posix() + "/" + name
        if content.count(old) != 1:
            raise ValueError("Cannot safely redirect the generated installation manifest")
        content = content.replace(old, workspace.as_posix() + "/" + name)
    return content


def reserve_consumer(build, token):
    if not re.fullmatch(r"[0-9a-f]{4}", token):
        raise ValueError("Unexpected resource workspace name")
    path = build / (".calendar-consumer-" + token)
    provider.mkdir(path, exist_ok=False)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cmake", "ctest", "build", "source", "workspace", "config", "generator"):
        parser.add_argument("--" + name, required=True)
    for name in ("dependency-prefix-hex", "icu-root", "toolchain", "target-triplet", "host-triplet"):
        parser.add_argument("--" + name, default="")
    args = parser.parse_args()
    dependency_prefix = bytes.fromhex(args.dependency_prefix_hex).decode("utf-8")
    source = Path(args.source).resolve(strict=True)
    raw_build, raw_workspace = Path(args.build), Path(args.workspace)
    state = source / "resources/.t"
    if raw_build.is_symlink() or raw_workspace.is_symlink() or state.is_symlink():
        raise ValueError("Build and test resource roots must be direct directories")
    build = raw_build.resolve(strict=True)
    workspace = raw_workspace.resolve(strict=True)
    state = state.resolve(strict=True)
    if build != (source / "build").resolve() or not workspace.is_relative_to(state) or workspace == state:
        raise ValueError("Installed consumer must use this project's build and resource workspace")
    cache = (build / "CMakeCache.txt").read_text()
    for name in ("BINDIR", "DATADIR", "DATAROOTDIR", "INCLUDEDIR", "LIBDIR"):
        match = re.search(r"^CMAKE_INSTALL_" + name + r":[^=]*=(.*)$", cache, re.M)
        if match and match[1] and (Path(match[1]).is_absolute() or PureWindowsPath(match[1]).is_absolute()
                                   or ".." in Path(match[1]).parts or ".." in PureWindowsPath(match[1]).parts):
            raise ValueError("Unsafe absolute or escaping install directory: " + name)
    script = redirect_manifest((build / "cmake_install.cmake").read_text(), build, workspace)
    prefix = workspace / "prefix"
    if prefix.exists() or prefix.is_symlink():
        raise FileExistsError("Test installation prefix already exists")
    private_script = workspace / "cmake_install.cmake"
    with provider.open(private_script, "x", encoding="utf-8") as output:
        output.write(script)
    env = {key: value for key, value in os.environ.items() if key.upper() != "DESTDIR"}
    manifest = build / "install_manifest.txt"
    before = manifest.read_bytes() if manifest.is_file() else None
    subprocess.run([args.cmake, "-DCMAKE_INSTALL_LOCAL_ONLY=ON", "-DCMAKE_INSTALL_PREFIX=" + str(prefix),
                    "-DCMAKE_INSTALL_CONFIG_NAME=" + args.config, "-P", str(private_script)], env=env, check=True)
    consumer = reserve_consumer(build, workspace.name)
    try:
        command = [args.cmake, "-G", args.generator, "-S", str(source / "tests/consumer"), "-B", str(consumer),
                   "-DCMAKE_PREFIX_PATH=" + str(prefix) + (";" + dependency_prefix if dependency_prefix else ""),
                   "-DICU_ROOT=" + args.icu_root, "-DCMAKE_BUILD_TYPE=" + args.config]
        for key, value in (("CMAKE_TOOLCHAIN_FILE", args.toolchain), ("VCPKG_TARGET_TRIPLET", args.target_triplet),
                           ("VCPKG_HOST_TRIPLET", args.host_triplet)):
            if value:
                command.append("-D" + key + "=" + value)
        subprocess.run(command, env=env, check=True)
        subprocess.run([args.cmake, "--build", str(consumer), "--config", args.config], env=env, check=True)
        subprocess.run([args.ctest, "--test-dir", str(consumer), "-C", args.config, "--output-on-failure"], env=env, check=True)
    finally:
        shutil.rmtree(consumer)
    after = manifest.read_bytes() if manifest.is_file() else None
    if before != after:
        raise RuntimeError("The original build installation manifest changed during the test")


if __name__ == "__main__":
    main()
