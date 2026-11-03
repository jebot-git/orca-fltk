#!/usr/bin/env python3
"""Real slicing/profile compatibility checks against the pinned Orca engine."""

import hashlib, json, re, pathlib, struct, subprocess, sys, zipfile

P = pathlib.Path
root = P(__file__).resolve().parents[1]
source = P(sys.argv[1]).resolve()
worker = root / "build-engine/orca-fltk-worker"
out = root / "artifacts/engine"
out.mkdir(parents=True, exist_ok=True)
resources = source / "resources"
profiles = resources / "profiles"
records = []
vertices = [
    (0, 0, 0),
    (8, 0, 0),
    (8, 8, 0),
    (0, 8, 0),
    (0, 0, 8),
    (8, 0, 8),
    (8, 8, 8),
    (0, 8, 8),
]
faces = [
    (0, 2, 1),
    (0, 3, 2),
    (4, 5, 6),
    (4, 6, 7),
    (0, 1, 5),
    (0, 5, 4),
    (1, 2, 6),
    (1, 6, 5),
    (2, 3, 7),
    (2, 7, 6),
    (3, 0, 4),
    (3, 4, 7),
]
model = out / "cube with spaces.stl"
with model.open("wb") as f:
    f.write(b"Orca FLTK fixture".ljust(80, b"\0"))
    f.write(struct.pack("<I", len(faces)))
    for face in faces:
        f.write(
            struct.pack("<12fH", 0, 0, 0, *[c for i in face for c in vertices[i]], 0)
        )
selected = {
    "machine": profiles / "Prusa/machine/Prusa MK3S 0.4 nozzle.json",
    "process": profiles / "Prusa/process/0.20mm Quality @MK3S.json",
    "filament": profiles / "Prusa/filament/Generic PLA @Prusa.json",
}
originals = {
    str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in selected.values()
}


def run(name, action="resolve", selection=None, expected=True, **extra):
    folder = out / name
    folder.mkdir(exist_ok=True)
    request = {
        "action": action,
        "input": str(model),
        "output": str(folder / "cube.gcode"),
        "resources": str(resources),
        "work_dir": str(folder),
        "profile_roots": [str(out / "user")],
        "profiles": {k: str(v) for k, v in (selection or selected).items()},
        **extra,
    }
    if expected and action == "slice":
        output = P(request["output"])
        if output.is_relative_to(folder):
            output.unlink(missing_ok=True)
    file = folder / "request.json"
    file.write_text(json.dumps(request, indent=2) + "\n")
    result = subprocess.run([worker, file], capture_output=True, text=True, timeout=120)
    (folder / "stdout.log").write_text(result.stdout)
    (folder / "stderr.log").write_text(result.stderr)
    events = []
    for line in result.stdout.splitlines():
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict) and "event" in value:
            events.append(value)
    if expected:
        assert result.returncode == 0, (name, events[-3:], result.stderr[-2000:])
        assert any(e["event"] == "result" for e in events), (name, events)
    else:
        assert result.returncode != 0 and any(e["event"] == "error" for e in events), (
            name,
            events,
        )
    records.append({"name": name, "passed": True, "exit": result.returncode})
    print("PASS", name, flush=True)
    return folder, events


folder, events = run(
    "native-compatibility-catalog", "catalog", machine=str(selected["machine"])
)
catalog = next(e for e in events if e["event"] == "result")["compatible"]
assert str(selected["process"]) in catalog["process"]
assert str(selected["filament"]) in catalog["filament"]
folder, _ = run("vendor-profile-resolution")
config = json.loads((folder / "effective-config.json").read_text())
assert abs(float(config["layer_height"]) - 0.2) < 1e-6
assert config["nozzle_diameter"] == ["0.4"]
assert len(config["printable_area"]) == 4
user = out / "user/process"
user.mkdir(parents=True, exist_ok=True)
custom = user / "User process ČR.json"
custom.write_text(
    json.dumps(
        {
            "type": "process",
            "name": "User process ČR",
            "inherits": "0.20mm Quality @MK3S",
            "from": "user",
            "layer_height": "0.24",
            "sparse_infill_density": "12%",
        },
        ensure_ascii=False,
        indent=2,
    )
    + "\n"
)
originals[str(custom)] = hashlib.sha256(custom.read_bytes()).hexdigest()
custom_selected = {**selected, "process": custom}
folder, _ = run("user-inheritance", selection=custom_selected)
config = json.loads((folder / "effective-config.json").read_text())
assert abs(float(config["layer_height"]) - 0.24) < 1e-6
folder, _ = run(
    "real-fff-slice",
    "slice",
    selection=custom_selected,
    overrides={"sparse_infill_density": "17%"},
)
gcode = folder / "cube.gcode"
assert gcode.stat().st_size > 1000
assert "OrcaSlicer" in gcode.read_text()
parsed = json.loads(
    subprocess.check_output(
        [root / "build/orca-fltk", "--inspect-gcode", gcode], text=True
    )
)
assert parsed["layers"] >= 10 and parsed["segments"] > 100
assert parsed["layers"] == int(
    re.search(r"; total layer number: (\d+)", gcode.read_text())[1]
)
before = hashlib.sha256(gcode.read_bytes()).hexdigest()
run("existing-output-is-preserved", "slice", expected=False, output=str(gcode))
assert hashlib.sha256(gcode.read_bytes()).hexdigest() == before
for name, parent in [("Cycle A", "Cycle B"), ("Cycle B", "Cycle A")]:
    (user / (name + ".json")).write_text(
        json.dumps(
            {"type": "process", "name": name, "inherits": parent, "layer_height": "0.2"}
        )
        + "\n"
    )
run(
    "inheritance-cycle-is-rejected",
    selection={**selected, "process": user / "Cycle A.json"},
    expected=False,
)
# Generic 3MF geometry exercises Orca's actual archive reader, not the STL viewer.
archive = out / "cube.3mf"
xml = (
    '<?xml version="1.0" encoding="UTF-8"?><model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources><object id="1" type="model"><mesh><vertices>'
    + "".join(f'<vertex x="{x}" y="{y}" z="{z}"/>' for x, y, z in vertices)
    + "</vertices><triangles>"
    + "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for a, b, c in faces)
    + '</triangles></mesh></object></resources><build><item objectid="1"/></build></model>'
)
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("3D/3dmodel.model", xml)
    z.writestr(
        "_rels/.rels",
        '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>',
    )
    z.writestr(
        "[Content_Types].xml",
        '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>',
    )
folder, events = run("native-3mf-import", "inspect", input=str(archive))
result = next(e for e in events if e["event"] == "result")
assert result["objects"] == 1 and P(result["mesh"]).is_file()
# Canonical Bambu template includes must survive headless profile resolution.
bambu = {
    "machine": profiles / "BBL/machine/Bambu Lab X1 Carbon 0.4 nozzle.json",
    "process": profiles / "BBL/process/0.20mm Standard @BBL X1C.json",
    "filament": profiles / "BBL/filament/Bambu PLA Basic @BBL X1C.json",
}
folder, _ = run("bambu-template-includes", selection=bambu)
config = json.loads((folder / "effective-config.json").read_text())
assert (
    len(config["machine_start_gcode"]) > 100 and "G28" in config["machine_start_gcode"]
)
# A user printer inherits the system printer's profile compatibility.
machine_folder = out / "user/machine"
machine_folder.mkdir(parents=True, exist_ok=True)
custom_machine = machine_folder / "My Prusa.json"
custom_machine.write_text(
    json.dumps(
        {
            "type": "machine",
            "name": "My Prusa",
            "inherits": "Prusa MK3S 0.4 nozzle",
            "from": "user",
        }
    )
    + "\n"
)
run(
    "user-printer-parent-compatibility",
    selection={**selected, "machine": custom_machine},
)
# Same-named user parents in different accounts must not shadow one another.
for account, value in [("account-one", "0.28"), ("account-two", "0.12")]:
    folder = out / "user" / account / "process"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "Shared base.json").write_text(
        json.dumps(
            {
                "type": "process",
                "name": "Shared base",
                "inherits": "0.20mm Quality @MK3S",
                "from": "user",
                "layer_height": value,
            }
        )
        + "\n"
    )
child = out / "user/account-one/process/Child.json"
child.write_text(
    json.dumps(
        {
            "type": "process",
            "name": "Scoped child",
            "inherits": "Shared base",
            "from": "user",
        }
    )
    + "\n"
)
folder, _ = run(
    "user-account-inheritance-scope", selection={**selected, "process": child}
)
config = json.loads((folder / "effective-config.json").read_text())
assert abs(float(config["layer_height"]) - 0.28) < 1e-6
folder, events = run(
    "stl-orientation-and-scale",
    "slice",
    transform={"rotation_deg": [90, 0, 0], "scale": 1.5},
)
transformed = json.loads(
    subprocess.check_output(
        [root / "build/orca-fltk", "--inspect-gcode", folder / "cube.gcode"], text=True
    )
)
assert transformed["layers"] == 60, transformed
run(
    "3mf-placement-edits-rejected",
    "inspect",
    expected=False,
    input=str(archive),
    transform={"rotation_deg": [90, 0, 0], "scale": 1},
)
for file, digest in originals.items():
    assert hashlib.sha256(P(file).read_bytes()).hexdigest() == digest, file
report = {
    "passed": True,
    "checks": records,
    "gcode_preview": parsed,
    "profile_files_unchanged": list(originals),
}
(out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
print("PASS all engine integration checks")
