# Architecture decisions

## Current boundary

The first executable is a **user homebrew NRO**, not a KIP/sysmodule. It owns a text UI and calls libnx `usb:hs` only for inspection. This isolates startup reliability from USB/network experimentation and gives a visible error path.

`usb:hs` exposes interfaces—not a ready-made network socket or Horizon network adapter. A future homebrew-only path would be:

```text
Android USB gadget -> usb:hs interface -> protocol driver -> Ethernet frames
    -> IP/DHCP/ARP/DNS implementation (or embedded stack) -> test client in this NRO
```

No component above registers a NIC with Horizon. Therefore games and ordinary system services cannot use it at this stage.

## Selected transport: RNDIS (S23 experimental target)

The physical S23 test selected **RNDIS**, subject to a control-plane validation. With USB tethering enabled, the Switch reports Samsung VID/PID `04E8:6863`, a control interface and a data interface (`0A/00/00`) with two 512-byte bulk endpoints. That control/data layout matches the Linux RNDIS gadget implementation and its RNDIS host matching rules. Without tethering, the query returns no interfaces.

This establishes USB enumeration, protocol identification, and RNDIS initialization. It does not establish Ethernet-frame transfer, IP, or Internet connectivity.

The physical test passed the initial control-plane acceptance criterion: **X** acquired interface 0, sent `REMOTE_NDIS_INITIALIZE_MSG` (24 bytes), received `REMOTE_NDIS_INITIALIZE_CMPLT` (52 bytes, request 1, status 0), sent `REMOTE_NDIS_HALT_MSG` (12 bytes), and closed the interface. Version 0.3.0 additionally sends a read-only `RNDIS_OID_802_3_CURRENT_ADDRESS` query after initialization. It still does not open a bulk endpoint or set a packet filter, so it cannot carry user traffic.

## Alternatives considered

| Candidate | What must be seen in Phase 2 | Decision status |
| --- | --- | --- |
| RNDIS | Control interface plus CDC data interface and bulk endpoints | Selected for this S23; requires RNDIS control + Ethernet encapsulation. |
| CDC-ECM | Ethernet CDC functional descriptors and a matching data interface | Not exposed by this S23 test. |
| CDC-NCM | NCM functional descriptors | Not exposed by this S23 test. |
| Custom Android companion protocol | A documented vendor interface and two bulk endpoints | Fallback; a normal app may not configure arbitrary USB gadget functions. |

The app now uses control transfers only for the validated RNDIS probe. It has not opened a data endpoint, enabled packet filtering, transmitted an Ethernet frame, or configured IP.

## Future system integration

Atmosphere supports custom sysmodules and service interception, but that alone does not establish a supported way to register a new network interface with Nintendo's network stack. A later investigation must identify the relevant Horizon services, IPC contracts, resource limits, and permissions on the target firmware. System-wide connectivity is explicitly **unproven**.
