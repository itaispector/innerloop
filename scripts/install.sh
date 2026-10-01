#!/bin/sh
# Builds, ad-hoc signs, and installs the InnerLoop HAL driver, then
# restarts coreaudiod so it picks up the new plugin. Run on macOS from the
# repo root (or anywhere — paths below are relative to this script).
#
# Pass a Developer ID to codesign with a real identity instead of ad-hoc:
#   SIGN_ID="Developer ID Application: Your Name (TEAMID)" ./scripts/install.sh

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DRIVER_DIR="$SCRIPT_DIR/../driver"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "error: this driver only builds/installs on macOS." >&2
    exit 1
fi

cd "$DRIVER_DIR"
make install
echo "Installed. Open Audio MIDI Setup.app to confirm 'InnerLoop' shows up."
