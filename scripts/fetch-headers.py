#!/usr/bin/env python3
from pathlib import Path
import urllib.request, hashlib, zipfile, subprocess

root = Path(__file__).resolve().parents[1]
base = root / ".deps"
prefix = base / "sdk/usr"
prefix.mkdir(parents=True, exist_ok=True)
sources = [
    (
        "eigen-5.0.1.zip",
        "https://gitlab.com/libeigen/eigen/-/archive/5.0.1/eigen-5.0.1.zip",
        "0dbb1f9e3aaad66f352c03227d8c983f6f0b49e0b07e71a7300f4abcc01aee12",
    ),
    (
        "CGAL-6.2.1.zip",
        "https://github.com/CGAL/cgal/releases/download/v6.2.1/CGAL-6.2.1.zip",
        "eebd737d9b7f0199647ba5c1f9f6c7a3d0651aacef9c2fd4dedc52da6c25edbe",
    ),
]
for name, url, digest in sources:
    archive = base / name
    if not archive.exists():
        urllib.request.urlretrieve(url, archive)
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest, name
    folder = base / (name.removesuffix(".zip") + "-src")
    folder.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(folder)
    source = next(p for p in folder.iterdir() if p.is_dir())
    build = base / (name.removesuffix(".zip") + "-build")
    subprocess.run(
        [
            "cmake",
            "-S",
            source,
            "-B",
            build,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_INSTALL_PREFIX=" + str(prefix),
            "-DEIGEN_BUILD_TESTING=OFF",
            "-DEIGEN_BUILD_BLAS=OFF",
            "-DEIGEN_BUILD_LAPACK=OFF",
        ],
        check=True,
    )
    subprocess.run(["cmake", "--install", build], check=True)
    print("Installed headers:", name, flush=True)
