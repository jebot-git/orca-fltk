#!/usr/bin/env python3
"""Prepare and audit a relocatable installation; never elevate or restart sessions."""

import argparse, hashlib, json, os, pathlib, re, shutil, subprocess, tempfile

P = pathlib.Path
root = P(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--prefix", type=P, default=P("/usr/local"))
p.add_argument("--dome", type=P)
a = p.parse_args()
assert a.prefix.is_absolute()
artifacts = root / "artifacts"
artifacts.mkdir(exist_ok=True)
stage = P(tempfile.mkdtemp(prefix="install-stage-", dir=artifacts))
env = {**os.environ, "DESTDIR": str(stage)}
for folder in ["build", "build-engine"]:
    subprocess.run(
        ["cmake", "--install", str(root / folder), "--prefix", str(a.prefix)],
        env=env,
        check=True,
        stdout=subprocess.DEVNULL,
    )
private = stage / str(a.prefix).lstrip("/") / "libexec/orca-fltk"
sdk = (root / ".deps/sdk").resolve()
lib = private / "lib"
lib.mkdir(exist_ok=True)
patchelf = shutil.which("patchelf")
if not patchelf:
    raise SystemExit("patchelf is required to prepare a relocatable install")


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def dependencies(path):
    output = subprocess.check_output(["ldd", str(path)], text=True)
    if "not found" in output:
        raise RuntimeError(output)
    pairs = re.findall(r"^\s*(\S+)\s+=>\s+(/\S+)", output, re.M)
    forbidden = [
        name
        for name, _ in pairs
        if any(
            token in name.lower() for token in ["libqt", "libgtk", "libwx", "webkit"]
        )
    ]
    if forbidden:
        raise RuntimeError("Unexpected GUI runtime dependency: " + str(forbidden))
    return pairs


for name, path in dependencies(root / "build-engine/orca-fltk-worker"):
    actual = P(path).resolve()
    if actual.is_relative_to(sdk):
        if name.startswith(("libc.so", "ld-linux")):
            raise RuntimeError("Cannot bundle a C library/loader replacement")
        shutil.copy2(actual, lib / name)
rpath = "$ORIGIN/lib:/opt/fltk-vr/bdf-runtime/lib:/opt/fltk-vr/toolkit/lib"
for path in [private / "orca-fltk", private / "orca-fltk-worker", *lib.iterdir()]:
    subprocess.run(
        [
            patchelf,
            "--force-rpath",
            "--set-rpath",
            ("$ORIGIN:" if path.parent == lib else "") + rpath,
            str(path),
        ],
        check=True,
    )
if a.dome:
    for name in ["usr/local/bin/dome-compositor", "opt/fltk-vr/bin/dome-compositor"]:
        path = stage / name
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(a.dome, path)
        subprocess.run(
            [
                patchelf,
                "--force-rpath",
                "--set-rpath",
                "/opt/fltk-vr/bdf-runtime/lib:/opt/fltk-vr/toolkit/lib:/usr/local/lib/domed/bitmap",
                str(path),
            ],
            check=True,
        )
closure = {
    name: dependencies(private / name) for name in ["orca-fltk", "orca-fltk-worker"]
}
for name in ["orca-fltk", "orca-fltk-worker"]:
    subprocess.run([str(private / name), "--version"], check=True)
# Retain notices for the statically linked Orca core and extracted runtime libraries.
doc = stage / str(a.prefix).lstrip("/") / "share/doc/orca-fltk/third-party"
source_config = (root / "build/CMakeCache.txt").read_text()
source = P(re.search(r"^ORCA_SOURCE_DIR:PATH=(.+)$", source_config, re.M)[1])
for base, label in [(source / "deps_src", "orca-deps"), (sdk / "usr/share/doc", "sdk")]:
    for notice in base.rglob("*"):
        if notice.is_file() and (
            notice.name.lower().startswith(("license", "licence", "copyright"))
        ):
            target = doc / label / notice.relative_to(base)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(notice, target)
            target.chmod(0o644)
shutil.copytree(root / "third_party/domed", doc / "domed", dirs_exist_ok=True)
for directory in doc.rglob("*"):
    if directory.is_dir():
        directory.chmod(0o755)
for path in stage.rglob("*"):
    if path.is_symlink():
        raise RuntimeError("Unexpected staged symlink: " + str(path))
    path.chmod(
        0o755
        if path.is_dir()
        or path.name in ["orca-fltk", "orca-fltk-worker", "dome-compositor"]
        else 0o644
    )
version = subprocess.check_output(
    ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
).strip()
files = [
    {
        "path": str(path.relative_to(stage)),
        "sha256": sha(path),
        "mode": path.stat().st_mode & 0o777,
    }
    for path in sorted(stage.rglob("*"))
    if path.is_file()
]
plan = {
    "stage": str(stage),
    "prefix": str(a.prefix),
    "commit": version,
    "files": files,
    "runtime_dependencies": closure,
    "dome_included": bool(a.dome),
}
file = artifacts / "install-plan.json"
file.write_text(json.dumps(plan, indent=2) + "\n")
print("Prepared", len(files), "files; no wx/GTK/Qt/WebKit dependencies. Plan:", file)
