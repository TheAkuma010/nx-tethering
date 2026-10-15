# Technical research

## Confirmed locally

- Installed header: `/opt/devkitpro/libnx/include/switch/services/usbhs.h` (inspected 2026-10-15). It declares initialization, interface queries, availability/state-change events, acquire/close, control transfers, endpoint open/close, and synchronous/asynchronous transfers.
- `UsbHsInterface` contains device/configuration descriptors; `UsbHsInterfaceInfo` contains one interface descriptor plus up to 15 input and 15 output endpoint descriptors.
- `usbHsIfCtrlXfer` and endpoint buffer functions require 0x1000-aligned address and size. They are intentionally unused until a protocol is selected.
- Version constraints in the declarations: async APIs are 2.0.0+, SMMU/report-ring support is 4.0.0+, and acquisition filters must be non-zero and unique on 7.0.0+.
- `/opt/devkitpro/examples/switch/usb/usbhs/source/main.c` is an installed reference for initialization, query, acquire, transfers, and cleanup.
- This workspace has `DEVKITPRO=/opt/devkitpro`, libnx libraries, `elf2nro` and `nacptool`. `DEVKITA64` is unset; `switch_rules` selects the devkitPro compiler.

## First physical evidence (2026-10-15)

- The supplied Switch screenshots show `usbHsInitialize: 0x00000000` and `usbHsQueryAllInterfaces: 0x00000000`: the NRO launches and the `usb:hs` service/query work on this console.
- With the S23 disconnected and USB tethering off, the scan reports no available interfaces. With USB tethering on, it reports Samsung `04E8:6863` as `E0/01/03` RNDIS control plus a `0A/00/00` data interface with bulk IN `0x82` and bulk OUT `0x01`, both with maximum packet size 512. This is the RNDIS control/data shape used by the Linux USB gadget implementation. RNDIS is therefore selected for the next implementation phase.
- The first 0.2.0 control probe did not send a message because it filtered only the alternate `02/02/FF` CDC-ACM RNDIS signature. The screenshots demonstrate that this S23 uses `E0/01/03`. Version 0.2.1 derives the exact filter from the detected control interface before acquiring it.
- The screenshots also exposed a UI defect in version 0.1.0: an ANSI cursor escape was emitted once per frame, while libnx's text console did not interpret it as intended. Version 0.1.1 redraws only on button input and contains no ANSI cursor escapes.
- No reliable interface count or Android descriptor can be recovered from the corrupted screen capture. A retest with 0.1.1 is required before inferring Android USB behavior.

## Confirmed upstream

- [libnx usbhs.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/usbhs.h) defines the host-service contract; [usbhs.c](https://github.com/switchbrew/libnx/blob/master/nx/source/services/usbhs.c) opens `usb:hs`, binds the current process on 2.0.0+, and dispatches queries/transfers through Horizon IPC.
- The [USB services wiki](https://switchbrew.org/wiki/USB_services) documents `usb:hs` and `usb:hs:a`; availability and permissions are Horizon/firmware dependent.
- [Atmosphere](https://github.com/Atmosphere-NX/Atmosphere) describes custom Stratosphere sysmodules, while [ams_mitm](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/ams_mitm.md) documents service interception. Neither proves automatic NIC registration.
- [Android USB documentation](https://source.android.com/docs/core/interaction/accessories/aoa) describes modes but does not guarantee OEM tethering functions. S23/Android 16 behavior must be measured.
- The Linux [RNDIS host driver](https://github.com/torvalds/linux/blob/master/drivers/net/usb/rndis_host.c) recognizes the wireless `E0/01/03` and CDC-ACM `02/02/FF` RNDIS signatures. Its [RNDIS gadget implementation](https://github.com/torvalds/linux/blob/master/drivers/usb/gadget/function/f_rndis.c) uses one control interface plus one `0A/00/00` data interface with two bulk endpoints, matching the S23 observation.
- Microsoft's [Remote NDIS messaging reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/remote-ndis-messaging) distinguishes control messages from data messages and identifies `REMOTE_NDIS_INITIALIZE_MSG`/completion as required control traffic.

## Hypotheses to test, not facts

1. Android USB tethering while the Switch hosts exposes RNDIS on this tested S23. Type-C role/power negotiation may still vary with cable, Android build, or future device.
2. A homebrew RNDIS driver can exchange Ethernet frames through `usb:hs` if its control initialization succeeds, but it still needs IP configuration/stack support.
3. A normal Android companion app may be unable to configure arbitrary USB gadget functions without privileged/OEM support.

## Open questions requiring the console

- Switch firmware, Atmosphere, and hbmenu versions.
- Results from `usbHsInitialize` and both scans.
- VID/PID, interface classes, endpoints, and whether Android tethering changes them.
- Whether cable/role negotiation provides a stable device to `usb:hs`.

## Prior-code audit

The working tree contained a Makefile that built `usb_tether_sysmodule.kip` via `elf2kip`. Its `main.c` used sysmodule threading alongside `appletMainLoop`, input, sockets, and nxlink. It did not provide a normal homebrew launch surface, which explains the startup risk. Deleted historical headers/sources were not restored or overwritten.
