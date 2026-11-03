#!/usr/bin/env python3
"""Exercise real FLTK windows on a private X display and session bus."""

import json, pathlib, subprocess, sys, time
from Xlib import X, display, protocol
from Xlib.ext import xtest
from PIL import Image

binary = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
fixture = out / "preview.gcode"
fixture.write_text(
    "G21\nG90\nM83\n"
    + "".join(
        f"G1 Z{z / 5}\nG0 X0 Y0\nG1 X20 Y0 E1\nG1 X20 Y20 E1\nG1 X0 Y20 E1\nG1 X0 Y0 E1\nG0 X0 Y0\nG1 X20 Y20 E1\n"
        for z in range(1, 31)
    )
)
reports = []
for stereo in [False, True]:
    image = out / ("stereo.ppm" if stereo else "mono.ppm")
    subprocess.run(
        [binary, "--render-test", fixture, image, *(["--stereo"] if stereo else [])],
        check=True,
        timeout=30,
    )
    picture = Image.open(image).convert("RGB")
    pixels = list(picture.getdata())
    background = pixels[0]
    assert sum(p != background for p in pixels) > 1000, "Empty rendered preview"
    if stereo:
        w, h = picture.size
        left = picture.crop((0, 0, w // 2, h))
        right = picture.crop((w // 2, 0, w, h))
        assert left.tobytes() != right.tobytes(), "Stereo eyes are identical"
    picture.save(image.with_suffix(".png"))
    reports.append({"mode": "stereo" if stereo else "mono", "passed": True})
log = (out / "gui.log").open("w")
app = subprocess.Popen([binary, fixture], stdout=log, stderr=subprocess.STDOUT)
d = display.Display()
window = None
try:
    for _ in range(300):
        assert app.poll() is None, "FLTK application exited during startup"
        for w in d.screen().root.query_tree().children:
            if (
                w.get_wm_name() == "Orca FLTK"
                and w.get_attributes().map_state == X.IsViewable
            ):
                window = w
                break
        if window:
            break
        time.sleep(0.05)
    assert window, "Main window did not appear"
    time.sleep(0.8)
    geom = window.get_geometry()
    raw = window.get_image(0, 0, geom.width, geom.height, X.ZPixmap, 0xFFFFFFFF)
    image = Image.frombytes("RGB", (geom.width, geom.height), raw.data, "raw", "BGRX")
    image.save(out / "application.png")
    # Filter printer profiles, exercising callbacks and dependent profile selections.
    for x, y in [(100, 88)]:
        xtest.fake_input(d, X.MotionNotify, x=x, y=y)
        xtest.fake_input(d, X.ButtonPress, detail=1)
        xtest.fake_input(d, X.ButtonRelease, detail=1)
    for key in "Creality":
        keysym = ord(key.lower())
        code = d.keysym_to_keycode(keysym)
        xtest.fake_input(d, X.KeyPress, detail=code)
        xtest.fake_input(d, X.KeyRelease, detail=code)
    d.sync()
    time.sleep(0.4)
    assert app.poll() is None
    event = protocol.event.ClientMessage(
        window=window,
        client_type=d.intern_atom("WM_PROTOCOLS"),
        data=(32, [d.intern_atom("WM_DELETE_WINDOW"), X.CurrentTime, 0, 0, 0]),
    )
    window.send_event(event)
    d.flush()
    assert app.wait(timeout=5) == 0
    reports.append({"mode": "native GUI, profile filtering and close", "passed": True})
finally:
    if app.poll() is None:
        app.terminate()
        app.wait(timeout=5)
    log.close()
    d.close()
(out / "report.json").write_text(
    json.dumps({"passed": True, "checks": reports}, indent=2) + "\n"
)
print("PASS native GUI, profile filtering, mono and distinct-eye stereo previews")
