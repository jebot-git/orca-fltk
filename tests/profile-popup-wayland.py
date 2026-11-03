#!/usr/bin/env python3
"""Verify native Wayland stereo identity with a private DomeD and synthetic XR."""

import json, os, pathlib, socket, subprocess, tempfile, time, urllib.request, sys

P = pathlib.Path
root = P(__file__).resolve().parents[1]
out = root / "artifacts/profile-popup-wayland"
out.mkdir(parents=True, exist_ok=True)
compositor = P(sys.argv[1]).resolve()
children = []
logs = []
try:
    with tempfile.TemporaryDirectory(prefix="orca-fltk-xr-") as directory:
        runtime = P(directory)
        runtime.chmod(0o700)
        read, write = os.pipe()

        def start(command, name, env, **kwargs):
            log = (out / (name + ".log")).open("w")
            logs.append(log)
            child = subprocess.Popen(
                list(map(str, command)),
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                **kwargs,
            )
            children.append(child)
            return child

        start(
            [
                "Xvfb",
                "-displayfd",
                write,
                "-screen",
                "0",
                "1280x900x24",
                "-nolisten",
                "tcp",
            ],
            "xvfb",
            os.environ.copy(),
            pass_fds=(write,),
        )
        os.close(write)
        with os.fdopen(read) as pipe:
            display = ":" + pipe.readline().strip()
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        env = {
            **os.environ,
            "DISPLAY": display,
            "XDG_RUNTIME_DIR": directory,
            "XDG_CONFIG_HOME": str(runtime / "config"),
            "XDG_DATA_HOME": str(runtime / "data"),
            "XDG_STATE_HOME": str(runtime / "state"),
            "HOME": str(runtime / "home"),
            "DOME_PREVIEW_PORT": str(port),
            "DOME_START_XR": "0",
            "DOME_START_TRAY": "0",
            "DOME_START_IME": "0",
            "DOME_LOGIN_SESSION": "0",
            "DOME_RENDERER": "vulkan",
            "RUST_LOG": "warn",
            "DBUS_SYSTEM_BUS_ADDRESS": "unix:path=" + str(runtime / "no-system-bus"),
            "XR_RUNTIME_JSON": str(runtime / "no-runtime.json"),
            "LD_LIBRARY_PATH": "/opt/fltk-vr/bdf-runtime/lib:/opt/fltk-vr/toolkit/lib",
        }
        for key in [
            "WAYLAND_DISPLAY",
            "WAYLAND_SOCKET",
            "FLTK_BACKEND",
            "LIBGL_ALWAYS_SOFTWARE",
            "GALLIUM_DRIVER",
            "MESA_LOADER_DRIVER_OVERRIDE",
        ]:
            env.pop(key, None)
        http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        base = f"http://127.0.0.1:{port}"

        def request(path, data=None):
            with http.open(
                urllib.request.Request(
                    base + path,
                    data=None if data is None else json.dumps(data).encode(),
                    headers={"Content-Type": "application/json"},
                ),
                timeout=2,
            ) as response:
                return json.load(response)

        def wait(fn):
            until = time.monotonic() + 30
            while time.monotonic() < until:
                try:
                    result = fn()
                    if result:
                        return result
                except (OSError, ValueError, KeyError, StopIteration):
                    pass
                time.sleep(0.05)
            raise AssertionError("Timed out; see artifacts/xr logs")

        start([compositor, "--backend", "nested"], "compositor", env)
        wait(lambda: request("/api/state"))

        def xr_socket():
            return next(
                str(p).removesuffix(".dome-session.json")
                for p in runtime.glob("*.dome-session.json")
                if json.loads(p.read_text())["realm"] == "xr"
            )

        client_env = {
            **env,
            "WAYLAND_DISPLAY": wait(xr_socket),
            "FLTK_BACKEND": "wayland",
        }
        client = start(
            [
                root / "build/slicer-profile-popup",
                "/usr/local/libexec/orca-fltk/resources",
                "/usr/local/libexec/orca-fltk/orca-fltk-worker",
                out,
            ],
            "preview",
            client_env,
        )
        assert client.wait(timeout=45) == 0, (out / "preview.log").read_text()
        print("PASS native Wayland unfiltered profile selectors")
finally:
    for child in reversed(children):
        if child.poll() is None:
            child.terminate()
    for child in reversed(children):
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
    for log in logs:
        log.close()
