#!/usr/bin/env python3
from pathlib import Path
import os, subprocess

root = Path(__file__).resolve().parents[1]
workspace = root.parent
dome = workspace / "dome-compositor"
previous = workspace / "update-artifacts/update-20261102"
rust = (
    dome / ".local-deps/toolchain/rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin"
)
env = {
    **os.environ,
    "PATH": str(rust) + ":" + os.environ["PATH"],
    "DOME_FLTK_PREFIX": str(previous / "toolkit"),
    "DOME_BDF_RUNTIME_PREFIX": str(previous / "runtime"),
    "PKG_CONFIG_PATH": str(previous / "runtime/lib/pkgconfig")
    + ":"
    + str(previous / "toolkit/lib/pkgconfig"),
    "LD_LIBRARY_PATH": str(previous / "runtime/lib")
    + ":"
    + str(previous / "toolkit/lib"),
    "CARGO_BUILD_JOBS": "1",
    "CARGO_TARGET_DIR": str(previous / "dome-target"),
    "CARGO_PROFILE_RELEASE_LTO": "thin",
    "CARGO_PROFILE_RELEASE_CODEGEN_UNITS": "1",
}
for name, args in [
    (
        "dome-stereo-tests.log",
        [
            "test",
            "--locked",
            "--release",
            "--features",
            "standalone,xr",
            "--lib",
            "stereo::tests",
        ],
    ),
    (
        "dome-integration-build.log",
        [
            "build",
            "--locked",
            "--release",
            "--features",
            "standalone,xr",
            "--bin",
            "dome-compositor",
        ],
    ),
]:
    with (root / name).open("w") as log:
        subprocess.run(
            ["cargo", *args],
            cwd=dome,
            env=env,
            check=True,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
    print(name, "passed", flush=True)
