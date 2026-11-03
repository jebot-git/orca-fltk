Orca FLTK
=========

Native FLTK slicer frontend for DomeD, with a dedicated worker built from
OrcaSlicer's slicing library. Printer, process and filament profiles use the
original Orca JSON files. The upstream GUI is not linked into either executable.

Features
--------

* STL import, orientation and uniform scale; OBJ/3MF import through Orca's reader.
* Original vendor and user profile selection, validation and byte-preserving export.
* Stable, literal profile menus, remembered selections per printer and cached
  compatibility results. Filtering keeps the active printer available.
* Canonical Orca inheritance, template includes, compatibility checks and final
  configuration assembly. User profiles can inherit installed vendor presets.
* One-plate FFF slicing, progress/cancellation and G-code export to a new file.
* Optional per-job layer height, infill and support overrides, without editing profiles.
* Layer-filtered toolpath preview, relative/absolute extrusion, tool changes,
  retractions, XY arcs and travel display.
* OctoPrint and Moonraker/Klipper network transfer, with progress and cancellation.
* A separate side-by-side sliced-object preview for DomeD's XR realm.
* Native BDF/PCF font appearance and FLTK file dialogs.

Build
-----

The engine source revision and borrowed appearance helpers are recorded in
``sources.json``. On the current Ubuntu/DomeD system::

    python3 scripts/build.py --prepare-sdk --jobs 2
    scripts/test-ui.sh
    build/orca-fltk

The SDK preparation extracts development packages into ``.deps``; it does not
install or upgrade system packages. Eigen and CGAL archives are checksum-verified.
An existing checkout can be passed with ``--source /path/to/OrcaSlicer`` and must
match the pinned revision. ``--frontend-only`` builds the viewer and data tests.
The frontend uses the patched FLTK 1.4.5 toolkit under ``/opt/fltk-vr/toolkit``.

Installation
------------

Prepare a relocatable stage after committing the source, then audit/apply it::

    python3 scripts/prepare-install.py --prefix /usr/local
    python3 scripts/install-stage.py artifacts/install-plan.json --verify-only
    sudo python3 scripts/install-stage.py artifacts/install-plan.json
    orca-fltk

The stage bundles required engine libraries privately. It installs no GTK, Qt,
wxWidgets or WebKit runtime. Per-file backups and an installation receipt record
what changed. The worker and pinned vendor profiles work without the build tree.
Private dependency package versions are recorded during SDK preparation in
``.deps/sdk-packages.json``.
For a user installation, pass ``--prefix "$HOME/.local"`` and omit ``sudo``.

The compositor patch for the stereo application identity must also be installed
for DomeD XR use. ``prepare-install.py --dome /path/to/patched/dome-compositor``
stages it at both existing DomeD binary paths. Start a new DomeD session after
installing a compositor update; the installer does not interrupt a running session.

Profiles and projects
---------------------

Select a printer explicitly, followed by a compatible process and filament.
The default sources are the pinned engine's ``resources/profiles`` directory and
``$XDG_CONFIG_HOME/OrcaSlicer/user`` (or ``~/.config/OrcaSlicer/user``). Add another
profile directory through the UI or repeat ``--profiles DIR`` on the command line.
An alternative complete Orca resource directory can be passed with ``--resources``.

Profile files are read-only. Export copies the original JSON bytes, retaining
unknown fields, arrays, macros and inheritance references. The relevant parent
profiles must also be available when importing an exported inherited profile.
Slicing resolution uses Orca's own loaders and configuration compatibility rules;
a source file inside a vendor manifest is never flattened by a substitute parser.
User inheritance cycles and ambiguous parents fail with a diagnostic.

The 3MF setting checkbox chooses whether embedded project settings take precedence.
Job overrides are applied after profile/project resolution. Each job keeps its
request, effective Orca configuration and worker log under
``$XDG_CACHE_HOME/orca-fltk/jobs``. Original Orca configuration directories are not
rewritten. A custom worker can be specified with ``--worker`` or
``ORCA_FLTK_WORKER``; resource discovery also accepts ``ORCA_FLTK_RESOURCES``.

Network transfer
----------------

After slicing or opening G-code, choose **Send G-code to printer**. Select
OctoPrint or Moonraker / Klipper, enter the server base URL (including http:// or
https://), and supply an API key when required. Reverse-proxy URL prefixes are
supported. Choose a remote filename and optionally an existing folder relative
to the server's G-code storage. The server may replace a file with the same name.

Uploads use the documented OctoPrint ``POST /api/files/local`` or Moonraker
``POST /server/files/upload`` multipart API. They store the file without starting
a print. Use **Cancel job** to stop an active transfer; a server may retain a
file it already received. A failed or cancelled request is not retried automatically.

HTTPS verifies the certificate and hostname. For a private certificate authority,
enter its PEM certificate file in the custom CA field. Redirects are reported as
errors. Transfer failures report HTTP status without echoing server response bodies.

Server URL, protocol, folder and CA file are remembered separately for each
printer. API keys stay in memory for the current application session and are not
written to settings, job requests or logs. Settings and profile selections are
stored atomically in ``$XDG_CONFIG_HOME/orca-fltk/settings.json`` (or
``~/.config/orca-fltk/settings.json``), with owner-only file permissions.
Original Orca profile files remain read-only. Compatibility results are discarded
when profile folders are rescanned. Cached results are only used for their printer.

Preview
-------

Drag to orbit, use the wheel to zoom, and press Space to fit the object.
The layer slider limits visible toolpaths. Travel moves can be shown separately.
Open existing G-code directly to preview without slicing.
Use Orient / scale for STL imports; 3MF placements are preserved.

The XR button launches a separate preview using ``dome-link --realm xr``. The
DomeD integration adds ``org.domed.OrcaPreview.Sbs`` to its explicit stereo client
identities; the normal slicer window remains mono. Desktop-only development can
render both eyes with ``--render-test FILE.gcode OUTPUT.ppm --stereo``.
XR support is stereoscopic window presentation with mouse orbit/zoom; it does not
provide a controller-based editing workspace or head-tracked object manipulation.

Current scope
-------------

This is an offline, single-plate FFF application. It does not implement Orca's
complete editing interface, multi-material painting, multi-plate slicing, printer
control/monitoring, cloud services, calibration wizards, parametric CAD editing,
text embossing or post-processing script execution. Multi-plate 3MF jobs are
rejected rather than silently combining plates. Existing profiles are reused;
there is no replacement profile format or automatic profile migration writer.

Verification
------------

In addition to the CTest data/process tests, the following run real slicing,
native FLTK UI rendering and profile-selection-to-toolpath integration::

    python3 tests/engine.py ../orca-upstream
    scripts/test-ui.sh
    dbus-run-session -- xvfb-run -a env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET \
        FLTK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 build/slicer-ui-flow \
        ../orca-upstream/resources build-engine/orca-fltk-worker \
        "artifacts/engine/cube with spaces.stl" artifacts/ui-flow

``tests/xr.py /path/to/patched/dome-compositor`` runs a private nested compositor
with synthetic XR tracking and checks native Wayland stereo eye routing. It
requires a private session bus (``dbus-run-session``). It does not test a headset.
Test reports and screenshots are saved under ``artifacts``.

CTest also exercises both upload protocols against local HTTP/HTTPS servers,
including exact file contents, authentication, path prefixes, TLS, cancellation,
redirect refusal and rejected requests. To include actual slicing and both
uploads through the FLTK application's transfer actions::

    python3 tests/transfer.py build/slicer-transfer-tests \
        dbus-run-session -- xvfb-run -a env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET \
        FLTK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 build/slicer-ui-flow \
        ../orca-upstream/resources build-engine/orca-fltk-worker \
        "artifacts/engine/cube with spaces.stl" artifacts/ui-flow-network

The UI flow also checks duplicate names, special characters, filtering, printer
switches, stale compatibility results, folder rescans and saved-selection restore.

Licensing
---------

Application and worker code are AGPL-3.0-or-later. Orca's original notices remain
in its pinned source tree. The copied DomeD bitmap appearance helpers are MIT;
their notices and source revision are retained in ``third_party/domed`` and
``sources.json``. No IotaSlicer engine code or prebuilt toolkit libraries are used.
