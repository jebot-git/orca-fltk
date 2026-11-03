#!/usr/bin/python3
"""Extract a private Debian SDK; never install or upgrade host packages."""

from pathlib import Path
import apt, apt_pkg, subprocess, json, os, urllib.request, hashlib, concurrent.futures, time

root = Path(__file__).resolve().parents[1]
base = root / ".deps"
archives = base / "debs"
sdk = base / "sdk"
archives.mkdir(parents=True, exist_ok=True)
sdk.mkdir(exist_ok=True)
names = [
    "libboost-filesystem-dev",
    "libboost-thread-dev",
    "libboost-log-dev",
    "libboost-locale-dev",
    "libboost-regex-dev",
    "libboost-iostreams-dev",
    "libboost-program-options-dev",
    "libboost-nowide-dev",
    "libboost-chrono-dev",
    "libboost-date-time-dev",
    "libboost-atomic-dev",
    "libtbb-dev",
    "libcereal-dev",
    "libnlopt-cxx-dev",
    "libgmp-dev",
    "libmpfr-dev",
    "libocct-data-exchange-dev",
    "libocct-visualization-dev",
    "libopencv-core-dev",
    "libopencv-imgproc-dev",
    "libopencv-imgcodecs-dev",
    "libassimp-dev",
    "libdraco-dev",
    "libnoise-dev",
    "libopenvdb-dev",
    "libimath-dev",
]
apt_pkg.config.set("APT::Install-Recommends", "false")
apt_pkg.config.set("APT::Install-Suggests", "false")
c = apt.Cache()
for name in names:
    c[name].mark_install(auto_fix=True, auto_inst=True, from_user=True)
packages = {p.name: p for p in c.get_changes() if p.marked_install or p.marked_upgrade}
for name in names:
    packages[name] = c[name]
records = []


def download(item):
    name, p = item
    v = p.candidate
    path = archives / Path(v.uri).name
    uri = v.uri.replace(
        "http://cz.archive.ubuntu.com", "https://archive.ubuntu.com"
    ).replace("http://", "https://")
    for attempt in range(3):
        try:
            if not path.exists():
                urllib.request.urlretrieve(uri, path)
            assert hashlib.sha256(path.read_bytes()).hexdigest() == v.sha256, name
            return name, v.version, path
        except Exception:
            path.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(1)


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    for name, version, path in pool.map(download, sorted(packages.items())):
        subprocess.run(["dpkg-deb", "--extract", path, sdk], check=True)
        records.append({"name": name, "version": version})
        print(name, version, flush=True)
# Development symlinks may refer to an already-installed runtime.
for f in sdk.rglob("*"):
    if f.is_symlink() and not f.exists():
        target = Path(os.readlink(f))
        candidate = Path("/usr/lib/x86_64-linux-gnu") / target.name
        if candidate.is_file():
            f.unlink()
            f.symlink_to(candidate)
for f in sdk.rglob("*"):
    if f.is_file() and not f.is_symlink() and (f.suffix in [".cmake", ".pc"]):
        try:
            s = f.read_text()
        except UnicodeDecodeError:
            continue
        s = (
            s.replace("prefix=/usr\n", "prefix=" + str(sdk / "usr") + "\n")
            .replace('"/usr/include', '"' + str(sdk / "usr/include"))
            .replace('"/usr/lib', '"' + str(sdk / "usr/lib"))
        )
        f.write_text(s)
(base / "sdk-packages.json").write_text(json.dumps(records, indent=2) + "\n")

# Imported CMake targets also name already-installed runtime files and helpers.
system = Path("/usr/lib/x86_64-linux-gnu")
local = sdk / "usr/lib/x86_64-linux-gnu"
for f in list(system.glob("*")) + list((system / "cmake").rglob("*")):
    if not f.is_file():
        continue
    target = local / f.relative_to(system)
    if not target.exists() and not target.is_symlink():
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(f)
# Orca's colour utilities include opencv.hpp but use only these modules.
# Keep the private SDK's umbrella headless; no GUI/DNN header bundles are needed.
umbrella = sdk / "usr/include/opencv4/opencv2/opencv.hpp"
umbrella.write_text(
    "#pragma once\n#include <opencv2/core.hpp>\n#include <opencv2/imgproc.hpp>\n#include <opencv2/imgcodecs.hpp>\n"
)
