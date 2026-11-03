#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
exec dbus-run-session -- xvfb-run -a -s '-screen 0 1440x1000x24' env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET FLTK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 LD_LIBRARY_PATH=/opt/fltk-vr/bdf-runtime/lib:/opt/fltk-vr/toolkit/lib uv run --no-project --with python-xlib --with pillow python tests/ui.py build/orca-fltk artifacts/ui
