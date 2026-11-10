# Apple Silicon to Apple Silicon VDM tool

This tool lets you get a serial console on an Apple Silicon device and reboot it remotely, using only another Apple Silicon device running macOS and a standard Type C cable.

## Disclaimer

I have no idea what I'm doing with IOKit and CoreFoundation -marcan

## Copyright

This is based on portions of [ThunderboltPatcher](https://github.com/osy/ThunderboltPatcher) and licensed under Apache-2.0.

* Copyright (C) 2019 osy86. All rights reserved.
* Copyright (C) 2021 The Asahi Linux Contributors

Thanks to t8012.dev and mrarm for assistance with the VDM and Ace2 host interface commands.

## Note about macOS 12 and 13

Access to the serial console device on macOS Monterey (12) and Ventura (13) was blocked by the `AppleSerialShim` extension. Update to later macOS version or look into the history of this document to generate a kernelcache without the `AppleSerialShim` extension.

## Notes for newer Apple Silicon (M3/M4/M5)

**The DFU port is not always RID 0.** `FindDevice()` used to hard-code RID 0 ("RID=0 seems to always be the right port"), which is wrong on some newer machines - on the M4 Mac mini, for example, the DFU-capable port is the middle rear one, RID 1, so the tool found no device at all. It now enumerates every `AppleHPM` port, picks the one that reports a connection, and prints what it found:

```
$ sudo ./macvdmtool status
Host type: J614cAP
Looking for HPM devices...
Found: RID=0 IOService:/.../hpm0@C/AppleHPMARMSPMI
Found: RID=1 IOService:/.../hpm1@A/AppleHPMARMSPMI (connected)
Connection: Source
Status: DBMa
```

You can pin a specific port with `-p`/`--rid` or `MACVDMTOOL_RID=<rid>`.

**Local serial mode may be rejected by the host.** On recent Apple Silicon hosts the host's own USB-PD controller rejects the `DVEn` routing command (status 3), so `/dev/cu.debug_console` never carries the target's output. Target-directed commands (`reboot`, `reboot debugusb`, `dfu`) still work. On such hosts use `reboot debugusb` and `/dev/cu.kis-100000-ch-0`.

**VDM replies are polled with a time budget.** The old code did 16 immediate register reads, which races controllers that post their reply a few hundred microseconds later. The reply wait is now bounded by `-t <seconds>` (default 3). If every command times out on a setup that used to work, unplug and replug the cable: the controller can keep the reply from an earlier USB-PD session in its receive register.

## Building

Install the XCode commandline tools and type `make`. `make clean` removes the objects; `make install` honours `PREFIX` and `DESTDIR`.

## Usage

Connect the two devices via their DFU ports. That's:
 - the rear port on MacBook Air and 13" MacBook Pro
 - the port next to the MagSafe connector on the 14" and 16" MacBook Pro
 - the port nearest to the power plug on Mac Mini
 - the middle rear port on the M4 Mac mini (RID 1)

Run `sudo ./macvdmtool status` first if you are not sure which port is the right one.

For debugusb mode USB 2.0-only cables like Apple includes for non-Magsafe Macbooks do work. For serial mode a *USB 3.0 compatible* (SuperSpeed) Type C cable. USB 2.0-only cables, including most cables meant for charging, will not work, as they do not have the required pins. Thunderbolt cables work too.

Run it as root (`sudo ./macvdmtool`).

```
Usage: ./macvdmtool <command>
Commands:
  serial - enter serial mode on both ends
  debugusb - enter debug usb mode on the target
  reboot - reboot the target
  reboot serial - reboot the target and enter serial mode
  reboot debugusb - reboot the target and enter debugusb mod
  dfu - put the target into DFU mode
  nop - do nothing
```

For serial mode use `/dev/cu.debug_console` on the local machine as your serial device. To use it with m1n1, `export M1N1DEVICE=/dev/cu.debug-console`.<br>
For debugusb mode use `/dev/cu.kis-100000-ch-0` on the local machine as your serial device. To use it with m1n1, `export M1N1DEVICE=/dev/cu.kis-100000-ch-0`.

For typical development, the command you want to use is `macvdmtool reboot serial` or `macvdmtool reboot debugusb`. This will reboot the target, and immediately put it back into serial or debugusb mode, with the right timing to make it work.
