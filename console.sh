#!/bin/sh
# Open whichever Apple debug console device currently exists.
# Ctrl-A q exits screen.
set -u

if [ -e /dev/cu.kis-100000-ch-0 ]; then
    dev=/dev/cu.kis-100000-ch-0
    mode="debugusb"
elif [ -e /dev/cu.debug-console ] || [ -e /dev/cu.debug_console ]; then
    dev=/dev/cu.debug-console
    mode="serial"
else
    echo "No debug console device yet."
    echo "Run: sudo ./macvdmtool reboot debugusb"
    exit 1
fi

echo "Console ($mode): $dev  (Ctrl-A then q to quit)"
sudo screen "$dev" 115200
