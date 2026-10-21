#!/usr/bin/env bash
# Flash build/wakeup.uf2 onto the Pico without pressing BOOTSEL.
#
#   sudo ./flash.sh        # or without sudo once host/60-pico-wakeup.rules is installed
#
# The running firmware uses its own USB ID, cafe:4012, which picotool doesn't
# look for by default, so -f needs --vid/--pid to find it and reboot it into
# BOOTSEL. If the Pico is already in BOOTSEL mode, a plain load is enough.
set -euo pipefail
cd "$(dirname "$0")"
uf2=build/wakeup.uf2
[[ -f $uf2 ]] || { echo "no $uf2 - build first: cmake --build build" >&2; exit 1; }

if picotool info >/dev/null 2>&1; then
    echo "Pico is in BOOTSEL mode, loading"
    exec picotool load -x "$uf2"
fi
echo "Rebooting the running firmware into BOOTSEL and loading"
exec picotool load -f --vid 0xcafe --pid 0x4012 -x "$uf2"
