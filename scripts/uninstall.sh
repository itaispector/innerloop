#!/bin/sh
# Removes the InnerLoop HAL driver and restarts coreaudiod.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DRIVER_DIR="$SCRIPT_DIR/../driver"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "error: this driver only builds/installs on macOS." >&2
    exit 1
fi

cd "$DRIVER_DIR"
make uninstall
echo "Uninstalled."
