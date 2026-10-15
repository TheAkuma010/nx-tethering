# NX Tethering

Experimental USB-host diagnostics for a Nintendo Switch running Atmosphere. This is **not yet reverse tethering** and does not expose a network interface to Horizon OS.

## Build

The development host must have devkitPro/libnx installed and `DEVKITPRO` set (this workspace uses `/opt/devkitpro`).

```sh
make
```

The output is `nx-tethering.nro`. Confirm it is an NRO before copying it:

```sh
file nx-tethering.nro
```

## First console test

1. Copy `nx-tethering.nro` to `sdmc:/switch/nx-tethering/nx-tethering.nro`.
2. Start hbmenu from a full game title (hold **R** while launching it), not Album applet mode.
3. Open the app. The first screen must remain open.
4. Press **A** with no accessory attached; capture the result codes. The screen redraws only after a button press; it must not continuously repeat text.
5. Connect the Galaxy S23 Ultra with a data-capable USB-C cable, enable USB tethering, press **A**, and capture every VID:PID, interface class/subclass/protocol, endpoint, and result code.
6. With the detected S23 still connected, press **X** once. This sends the reversible RNDIS initialize/control probe, reads its reply, sends RNDIS HALT, and closes the USB interface. Capture the result screen.
7. Press **PLUS** to exit. The app should return to hbmenu cleanly.

Do not install this as a sysmodule and do not copy anything to NAND. `usb:hs` may refuse interfaces already owned by Horizon services; report the result rather than retrying aggressively.

See [docs/ROADMAP.md](docs/ROADMAP.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), and [docs/RESEARCH.md](docs/RESEARCH.md).
