#!/usr/bin/env python3
"""Build the pinned native frontend and GUI-free Orca slicing worker."""

import argparse, json, pathlib, subprocess

P = pathlib.Path
root = P(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--jobs", type=int, default=2)
p.add_argument("--source", type=P)
p.add_argument("--prepare-sdk", action="store_true")
p.add_argument("--frontend-only", action="store_true")
a = p.parse_args()


def run(args):
    subprocess.run(list(map(str, args)), cwd=root, check=True)


pin = json.loads((root / "sources.json").read_text())["orca"]
source = a.source or root.parent / "orca-upstream"
if not (source / ".git").exists():
    source = root / ".deps/orca"
    source.parent.mkdir(exist_ok=True)
    if not (source / ".git").exists():
        run(
            [
                "git",
                "clone",
                "--filter=blob:none",
                "--no-checkout",
                pin["repository"],
                source,
            ]
        )
    run(["git", "-C", source, "sparse-checkout", "init", "--cone"])
    run(
        [
            "git",
            "-C",
            source,
            "sparse-checkout",
            "set",
            "src",
            "deps_src",
            "deps",
            "cmake",
            "resources/profiles",
        ]
    )
    run(["git", "-C", source, "fetch", "--depth", "1", "origin", pin["commit"]])
    run(["git", "-C", source, "checkout", "--detach", pin["commit"]])
actual = subprocess.check_output(
    ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
).strip()
if actual != pin["commit"]:
    raise SystemExit("Engine source must match sources.json: " + pin["commit"])
if subprocess.check_output(
    ["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"],
    text=True,
).strip():
    raise SystemExit(
        "Pinned engine checkout contains modifications; use a clean source tree"
    )
if a.prepare_sdk:
    run(["/usr/bin/python3", root / "scripts/prepare-sdk.py"])
    run(["python3", root / "scripts/fetch-headers.py"])
run(
    [
        "cmake",
        "-S",
        root,
        "-B",
        root / "build",
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_TESTING=ON",
        "-DORCA_SOURCE_DIR=" + str(source.resolve()),
    ]
)
run(["cmake", "--build", root / "build", "--parallel", a.jobs])
run(["ctest", "--test-dir", root / "build", "--output-on-failure"])
if not a.frontend_only:
    run(
        [
            "cmake",
            "-S",
            root / "engine",
            "-B",
            root / "build-engine",
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_PREFIX_PATH=" + str(root / ".deps/sdk/usr"),
            "-DORCA_SOURCE_DIR=" + str(source.resolve()),
            "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG",
        ]
    )
    run(["cmake", "--build", root / "build-engine", "--parallel", a.jobs])
