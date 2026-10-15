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

This establishes USB enumeration and protocol identification only. It does not yet establish RNDIS initialization, Ethernet-frame transfer, IP, or Internet connectivity.

Version 0.2.0 adds a narrowly scoped validation path: **X** queries an available CDC-ACM/RNDIS control interface using a non-zero filter, acquires it, sends `REMOTE_NDIS_INITIALIZE_MSG` over CDC `SEND_ENCAPSULATED_COMMAND`, obtains `GET_ENCAPSULATED_RESPONSE`, then sends `REMOTE_NDIS_HALT_MSG` and closes the interface. It does not open either bulk data endpoint or set a packet filter, so it cannot carry user traffic. A successful `REMOTE_NDIS_INITIALIZE_CMPLT` with status zero is the next acceptance criterion.

## Alternatives considered

| Candidate | What must be seen in Phase 2 | Decision status |
| --- | --- | --- |
| RNDIS | Control interface plus CDC data interface and bulk endpoints | Selected for this S23; requires RNDIS control + Ethernet encapsulation. |
| CDC-ECM | Ethernet CDC functional descriptors and a matching data interface | Not exposed by this S23 test. |
| CDC-NCM | NCM functional descriptors | Not exposed by this S23 test. |
| Custom Android companion protocol | A documented vendor interface and two bulk endpoints | Fallback; a normal app may not configure arbitrary USB gadget functions. |

The app only reads descriptors today. It neither acquires an interface nor sends transfers, avoiding guesses about a protocol.

## Future system integration

Atmosphere supports custom sysmodules and service interception, but that alone does not establish a supported way to register a new network interface with Nintendo's network stack. A later investigation must identify the relevant Horizon services, IPC contracts, resource limits, and permissions on the target firmware. System-wide connectivity is explicitly **unproven**.
