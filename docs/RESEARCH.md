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
- The 0.2.1 rerun reached `usbHsQueryAvailableInterfaces: 0x00000000` but `usbHsAcquireUsbIf: 0x0000025A`. Source review found a local bug: it passed the discovery-only `usbHsQueryAllInterfaces` entry to `usbHsAcquireUsbIf`, rather than the entry returned by `usbHsQueryAvailableInterfaces`. On this service, discovery entries use an invalid acquisition ID. Version 0.2.2 passes the available entry and reports the interface number/ID before acquisition.
- The 0.2.2 physical probe acquired interface 0 (ID `1342701568`), sent `REMOTE_NDIS_INITIALIZE_MSG` successfully (24 bytes), received `REMOTE_NDIS_INITIALIZE_CMPLT` (type `0x80000002`, length 52, request 1, status 0), and sent `REMOTE_NDIS_HALT_MSG` successfully (12 bytes). This validates the S23 RNDIS control plane. It is not evidence of Ethernet, DHCP, DNS, or Internet connectivity.
- The 0.3.0 physical probe successfully queried `RNDIS_OID_802_3_CURRENT_ADDRESS`: the S23 returned RNDIS adapter MAC `BA:99:FF:7F:BD:23` in a 30-byte response. The control plane is therefore validated beyond initialization. It remains independent of bulk data transfer.
- The 0.4.0 physical bulk probe acquired data interface 1 (ID `1342767105`) and opened bulk IN endpoint `0x82` and bulk OUT endpoint `0x01`; all USB Host calls returned zero. This proves endpoint-session creation only, not a bulk transfer or Ethernet-frame exchange.
- The 0.5.0 physical probe set `RNDIS_OID_GEN_CURRENT_PACKET_FILTER` to `0x0000000F`, received the 16-byte successful completion, then cleared the filter to zero with another successful 16-byte completion before HALT. The S23 accepts the state transition that enables RNDIS data flow; no endpoint was opened during this particular probe.
- The first 0.6.0 DHCP probe sent its 386-byte RNDIS-framed DHCPDISCOVER successfully. Bulk IN completed successfully but with zero bytes, which is not an Ethernet packet and cannot be interpreted as a DHCPOFFER. Version 0.6.1 treats this as an ignorable empty completion and reposts the receive buffer before timing out.
- Two supplied screenshots labeled 0.6.1 still showed the pre-retry message and no `DHCP repost receive` log. The local 0.6.1 artifact contains the retry strings, so version 0.6.2 is distributed under a distinct filename to rule out hbmenu/file-copy caching.
- The 0.6.2 screenshot confirmed the correct build executed, ruling out cache/substitution. It still reached the `not a complete RNDIS Ethernet packet` branch, which means its received completion was non-empty (the zero-length retry branch was not entered). Version 0.6.3 prints the received byte count and first 32 bytes before parsing; no protocol conclusion is made until this payload is identified.
- The 0.6.3 receive begins `01 00 00 00 ... 24 00 00 00 ...`, consistent with a RNDIS packet message and the expected data-offset field (`0x24`). The prior parser incorrectly required every inbound Ethernet frame to be at least DHCP/BOOTP size before inspecting it. Version 0.6.4 first validates generic RNDIS framing, then reports Ethernet addresses/type and handles ARP separately from IPv4/DHCP.
- The 0.6.4 generic parser identified the frame as Ethernet `0x86DD` (IPv6), not a DHCPv4 reply. This is direct physical evidence of inbound RNDIS frame delivery, complementing the successful 386-byte outbound DHCPDISCOVER. Version 0.6.5 keeps listening after unrelated frames rather than treating the first frame as the final response.
- Three 0.6.5 runs showed why the receive loop must not stop on unrelated traffic: the first two received only repeating IPv6 multicast frames; the third received a DHCPv4 DHCPOFFER (message type 2) offering `10.248.48.125` from server `10.248.48.1`. This is evidence of successful RNDIS/Ethernet/IPv4/UDP/DHCPDISCOVER-to-OFFER transport. It does not yet prove a DHCPACK, configured IP address, routing, DNS, or Internet access.
- A physical 0.7.0 run then sent DHCPREQUEST after a type-2 offer and received DHCP message type 5 (DHCPACK), printing validation address `10.248.48.45`. The code deliberately only reports this result: it does not configure that address on Horizon or claim usable Internet connectivity.
- Version 0.8.0 will parse DHCP options 1 (subnet mask), 3 (router), 6 (DNS), and 51 (lease time) from a DHCPACK. It then uses only option 3 to construct a constrained ARP request. A valid ARP reply must identify that router as sender and the DHCPACK client address as target before it is recorded as a gateway MAC.
- Two physical 0.8.0 runs delivered only IPv6 multicast packets during the DHCP discovery window, with no DHCPOFFER. This confirms the receive path remained active but is not evidence of DHCP failure. Version 0.8.1 retransmits DHCPDISCOVER after completed unrelated receives before timing out.
- A physical 0.8.1 run obtained DHCP address `10.248.48.177`, subnet `255.255.255.0`, router/DNS `10.248.48.125`, lease 3599 seconds, and a matching ARP reply after requesting the router. This proves that the returned DHCP configuration is coherent at Ethernet/ARP layers. Version 0.9.0 adds a single local ICMP echo request with source set to the DHCPACK address and destination set to that router; it validates the reply's IPv4 endpoints plus ICMP identifier and sequence.
- The physical 0.9.0 run received a matching ICMP echo reply from the DHCP router. This validates the custom Ethernet, ARP, IPv4, and ICMP path locally. Version 0.10.0 adds a constrained UDP DNS request to the DHCP option-6 server for `example.com`, with DNS transaction ID, IPv4 endpoints, UDP ports, response bit, and success code validated before displaying A records.
- A physical 0.10.0 run received a valid DNS response with two A records for `example.com`: `172.66.144.243` and `104.20.34.239`. This proves the tether can carry the custom UDP DNS request and return an Internet-derived DNS answer, but it is not evidence of TCP or HTTP connectivity.
- Version 0.11.0 retains the first A record from a validated DNS response and uses a fixed ephemeral source port and sequence number for a single port-80 SYN. A valid result requires a matching SYN-ACK acknowledgement. It sends RST+ACK after validation so no application request or open connection remains.
- The screenshots also exposed a UI defect in version 0.1.0: an ANSI cursor escape was emitted once per frame, while libnx's text console did not interpret it as intended. Version 0.1.1 redraws only on button input and contains no ANSI cursor escapes.
- No reliable interface count or Android descriptor can be recovered from the corrupted screen capture. A retest with 0.1.1 is required before inferring Android USB behavior.

## Confirmed upstream

- [libnx usbhs.h](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/usbhs.h) defines the host-service contract; [usbhs.c](https://github.com/switchbrew/libnx/blob/master/nx/source/services/usbhs.c) opens `usb:hs`, binds the current process on 2.0.0+, and dispatches queries/transfers through Horizon IPC.
- The [USB services wiki](https://switchbrew.org/wiki/USB_services) documents `usb:hs` and `usb:hs:a`; availability and permissions are Horizon/firmware dependent.
- [Atmosphere](https://github.com/Atmosphere-NX/Atmosphere) describes custom Stratosphere sysmodules, while [ams_mitm](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/ams_mitm.md) documents service interception. Neither proves automatic NIC registration.
- [Android USB documentation](https://source.android.com/docs/core/interaction/accessories/aoa) describes modes but does not guarantee OEM tethering functions. S23/Android 16 behavior must be measured.
- The Linux [RNDIS host driver](https://github.com/torvalds/linux/blob/master/drivers/net/usb/rndis_host.c) recognizes the wireless `E0/01/03` and CDC-ACM `02/02/FF` RNDIS signatures. Its [RNDIS gadget implementation](https://github.com/torvalds/linux/blob/master/drivers/usb/gadget/function/f_rndis.c) uses one control interface plus one `0A/00/00` data interface with two bulk endpoints, matching the S23 observation.
- Microsoft's [Remote NDIS messaging reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/remote-ndis-messaging) distinguishes control messages from data messages and identifies `REMOTE_NDIS_INITIALIZE_MSG`/completion as required control traffic.
- Linux's [RNDIS host implementation](https://github.com/torvalds/linux/blob/master/drivers/net/usb/rndis_host.c) builds a `RNDIS_MSG_QUERY` using OID `RNDIS_OID_802_3_CURRENT_ADDRESS` and a query information-buffer offset of 20. Version 0.3.0 follows that layout for a read-only MAC-address check.
- The Linux [RNDIS host driver](https://github.com/torvalds/linux/blob/master/drivers/net/usb/rndis_host.c) sets a non-zero `RNDIS_OID_GEN_CURRENT_PACKET_FILTER` before data transfer; the Linux [RNDIS gadget](https://github.com/torvalds/linux/blob/master/drivers/usb/gadget/function/rndis.c) documents this as the state-changing action that starts/stops packet flow. Version 0.5.0 enables it only long enough to validate the completion, then clears it.
- [RFC 2131](https://www.rfc-editor.org/info/rfc2131/) defines DHCP's BOOTP-format client/server exchange; [IANA's DHCP parameter registry](https://www.iana.org/assignments/bootp-dhcp-parameters) assigns option 53 (message type), 55 (parameter request list), and 61 (client identifier). Version 0.6.0 sends only DHCPDISCOVER and treats DHCPOFFER as transport evidence, not as a configured lease.

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
