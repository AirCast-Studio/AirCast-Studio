#!/usr/bin/env bash
# Dev launcher: run the locally installed AirCast Studio (.install) with its own libobs.
# Dev builds have no rpath, so without this it would load the system OBS's libobs.
set -euo pipefail

# Started from VS Code's snap terminal: drop the snap's GTK/GSettings/locale paths. Its older GNOME
# schemas crash Qt's GTK theme at startup ("...xsettings does not contain a key named 'antialiasing'").
# SNAP_NAME isn't always set (e.g. VS Code started from the desktop), but the snap always leaves *_VSCODE_SNAP_ORIG.
if [[ "${SNAP_NAME:-}" == code || -n "${GTK_PATH_VSCODE_SNAP_ORIG+x}" ]]; then
  unset GSETTINGS_SCHEMA_DIR GTK_PATH GTK_EXE_PREFIX GTK_IM_MODULE_FILE GDK_PIXBUF_MODULE_FILE \
        GDK_PIXBUF_MODULEDIR GIO_MODULE_DIR LOCPATH
  export XDG_DATA_DIRS="${XDG_DATA_DIRS_VSCODE_SNAP_ORIG:-/usr/local/share:/usr/share}"
  export XDG_CONFIG_DIRS="${XDG_CONFIG_DIRS_VSCODE_SNAP_ORIG:-/etc/xdg}"
fi

here="$(cd "$(dirname "$0")/.." && pwd)/.install"
export LD_LIBRARY_PATH="$here/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$here/bin"
exec ./obs "$@"
