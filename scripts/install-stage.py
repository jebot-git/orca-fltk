#!/usr/bin/env python3
"""Apply a verified Orca FLTK stage with per-file backups and rollback."""

import argparse, datetime, hashlib, json, os, pathlib, shutil, tempfile, subprocess

P = pathlib.Path
root = P(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument("plan", type=P)
p.add_argument("--verify-only", action="store_true")
a = p.parse_args()
plan = json.loads(a.plan.read_text())
stage = P(plan["stage"])
prefix = P(plan["prefix"])
files = plan["files"]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


allowed = (
    str(prefix).lstrip("/") + "/libexec/orca-fltk/",
    str(prefix).lstrip("/") + "/share/doc/orca-fltk/",
)
exact = {
    str(prefix).lstrip("/") + "/bin/orca-fltk",
    str(prefix).lstrip("/") + "/share/applications/org.domed.OrcaSlicer.desktop",
}
if plan["dome_included"]:
    exact |= {"usr/local/bin/dome-compositor", "opt/fltk-vr/bin/dome-compositor"}
for item in files:
    name = item["path"]
    path = P(name)
    assert (
        not path.is_absolute()
        and ".." not in path.parts
        and (name.startswith(allowed) or name in exact)
    ), name
    assert not (stage / path).is_symlink() and sha(stage / path) == item["sha256"], name
    assert item["mode"] in [0o644, 0o755], name
if a.verify_only:
    print("Verified", len(files), "staged files")
    raise SystemExit(0)
if str(prefix).startswith("/usr/") and os.geteuid() != 0:
    raise SystemExit("System installation requires native sudo authentication")
os.umask(0o022)
backup = (
    P("/var/backups/orca-fltk") if os.geteuid() == 0 else root / "artifacts/backups"
) / datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
backup.mkdir(parents=True)
changed = []


def replace(target, source, mode):
    target.parent.mkdir(parents=True, exist_ok=True, mode=0o755)
    fd, name = tempfile.mkstemp(prefix="." + target.name + ".orca-", dir=target.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            with source.open("rb") as inp:
                shutil.copyfileobj(inp, out)
            out.flush()
            os.fsync(out.fileno())
            os.fchmod(out.fileno(), mode)
        os.replace(name, target)
    finally:
        P(name).unlink(missing_ok=True)


try:
    for item in files:
        target = P("/") / item["path"]
        assert not any(p.is_symlink() for p in target.parents), (
            "Symlinked install directory: " + str(target)
        )
        if (
            target.is_file()
            and not target.is_symlink()
            and sha(target) == item["sha256"]
            and target.stat().st_mode & 0o777 == item["mode"]
        ):
            continue
        exists = target.exists() or target.is_symlink()
        if exists:
            if target.is_symlink():
                raise RuntimeError(
                    "Refusing to replace a launcher/library symlink: " + str(target)
                )
            saved = backup / item["path"]
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, saved)
        changed.append({"path": item["path"], "previous": exists})
        replace(target, stage / item["path"], item["mode"])
    for item in files:
        assert sha(P("/") / item["path"]) == item["sha256"], item["path"]
    # Keep the suite's compositor inventory accurate when this stage updates it.
    receipt = P("/opt/fltk-vr/install-manifest.json")
    if plan["dome_included"] and receipt.is_file():
        name = str(receipt).lstrip("/")
        saved = backup / name
        saved.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(receipt, saved)
        changed.append({"path": name, "previous": True})
        data = json.loads(receipt.read_text())
        by_path = {x["path"]: x for x in data}
        by_path.update(
            {
                x["path"]: x
                for x in files
                if x["path"]
                in ["usr/local/bin/dome-compositor", "opt/fltk-vr/bin/dome-compositor"]
            }
        )
        temp = backup / "updated-manifest.json"
        temp.write_text(json.dumps(list(by_path.values()), indent=2) + "\n")
        replace(receipt, temp, 0o644)
    (backup / "changes.json").write_text(json.dumps(changed, indent=2) + "\n")
    (backup / "install-plan.json").write_text(json.dumps(plan, indent=2) + "\n")
except BaseException:
    for item in reversed(changed):
        target = P("/") / item["path"]
        if item["previous"]:
            source = backup / item["path"]
            replace(target, source, source.stat().st_mode & 0o777)
        else:
            target.unlink(missing_ok=True)
    raise
receipt = prefix / "share/doc/orca-fltk/install-receipt.json"
receipt.parent.mkdir(parents=True, exist_ok=True)
receipt.write_text(
    json.dumps(
        {
            "commit": plan["commit"],
            "backup": str(backup),
            "verified_files": len(files),
            "dome_included": plan["dome_included"],
        },
        indent=2,
    )
    + "\n"
)
receipt.chmod(0o644)
if shutil.which("update-desktop-database"):
    subprocess.run(
        ["update-desktop-database", str(prefix / "share/applications")], check=True
    )
print("Installed and verified", len(files), "files. Backup:", backup)
print("Running sessions were left running.")
