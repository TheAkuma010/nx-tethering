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

The physical test passed the initial control-plane acceptance criterion: **X** acquired interface 0, sent `REMOTE_NDIS_INITIALIZE_MSG` (24 bytes), received `REMOTE_NDIS_INITIALIZE_CMPLT` (52 bytes, request 1, status 0), queried the current adapter MAC, set/cleared `RNDIS_OID_GEN_CURRENT_PACKET_FILTER`, sent `REMOTE_NDIS_HALT_MSG` (12 bytes), and closed the interface. The **Y** probe acquired data interface 1 and opened/closed bulk IN `0x82` and OUT `0x01` successfully.

Version 0.6.1 adds **R**, a bounded data-plane probe. It initializes the same RNDIS session, queries the session MAC, enables the packet filter, posts one aligned bulk-IN receive, and sends one Ethernet/IPv4/UDP DHCPDISCOVER in an RNDIS packet through bulk OUT. It waits up to six seconds for an inbound packet, ignoring and reposting zero-length bulk completions, and parses a DHCPOFFER if present. Cleanup clears the packet filter, sends HALT, closes endpoints/interfaces, and frees all buffers. It intentionally does not send DHCPREQUEST, accept a lease, or configure the Switch IP address.

The first non-empty inbound frame was an RNDIS/Ethernet IPv6 multicast (`EtherType 0x86DD`), not DHCPv4. Version 0.6.5 no longer ends the probe on a valid but unrelated frame: it logs it, reposts bulk IN, and continues for up to eight seconds. This establishes that the RNDIS data plane delivers frames in both directions; DHCPv4 lease acquisition remains unproven.

The third physical 0.6.5 run received a valid DHCPv4 DHCPOFFER: offered address `10.248.48.125`, server identifier `10.248.48.1`, DHCP message type 2. This proves the Android tether's IPv4 DHCP service is reachable over the custom RNDIS transport. The lease has **not** been accepted (no DHCPREQUEST/DHCPACK) and no Switch process has been configured with that address.

Version 0.7.0 keeps this boundary while validating the next DHCP transition. After retaining a type-2 offer containing a server identifier, it sends a DHCPREQUEST with that address and server identifier, then accepts only a type-5 DHCPACK as diagnostic evidence. It does not apply the resulting address, subnet, route, DNS, or lease timers anywhere on the Switch.

The physical S23 run of version 0.7.0 completed that exchange: it logged `DHCPREQUEST bulk send`, then received DHCP message type 5 and printed `DHCPACK accepted for validation only: 10.248.48.45`. The app subsequently cleared the packet filter and halted RNDIS successfully. This is end-to-end evidence for RNDIS plus DHCP DISCOVER/OFFER/REQUEST/ACK; it remains separate from configuring a homebrew IP stack or Horizon networking.

Version 0.8.0 parses the DHCPACK options needed for a later homebrew IPv4 stack (subnet mask, router, DNS, and lease time). If a router option is present, it sends exactly one ARP request for that router and accepts only a reply whose sender protocol address is the router and whose target protocol address is the DHCPACK address. No IP address is installed and no IP packet is sent in this phase.

The two initial physical 0.8.0 attempts received valid RNDIS IPv6 multicast frames but no DHCPOFFER before the eight-second window. Version 0.8.1 keeps the same bounded diagnostic but retransmits DHCPDISCOVER twice after completed, unrelated receives and extends only the discovery wait to twelve seconds. This accommodates a delayed Android DHCP service without treating the IPv6 traffic as failure or proceeding to ARP without a DHCPACK.

The subsequent 0.8.1 physical test completed the full guarded path: it obtained `10.248.48.177`, mask `255.255.255.0`, router/DNS `10.248.48.125`, and a 3599-second lease; it then received a matching ARP reply from the requested router. Version 0.9.0 uses that resolved MAC only to emit one ICMP echo request to the local DHCP router, accepting a reply only when IPv4 source/destination and ICMP identifier/sequence match. It does not contact an external address.

The physical 0.9.0 run received the requested ICMP echo reply from the DHCP router, demonstrating a working local IPv4 packet exchange after DHCP and ARP. Version 0.10.0 then sends one UDP DNS A query for `example.com` to the DNS server supplied in the DHCPACK (normally the tether gateway), parses only a matching DNS response, and prints any A records. It still emits no HTTP request and does not configure Horizon.

The physical 0.10.0 run received and parsed the DNS response, returning `172.66.144.243` and `104.20.34.239` for `example.com`. This demonstrates that the DHCP-provided DNS path can resolve a public name through the Android tether. It does not yet prove a TCP handshake, HTTP exchange, or a usable network interface for Horizon.

Version 0.11.0 takes the first A record from that validated DNS response and sends exactly one TCP SYN to port 80. It accepts only a SYN-ACK with the expected IPv4 endpoints, ports, acknowledgement number, and SYN/ACK flags; it immediately sends RST+ACK to release the half-open test connection. No HTTP payload is transmitted in this phase.

The physical 0.11.0 run received the matching SYN-ACK from the resolved `example.com` address and sent RST cleanup successfully. Version 0.12.0 completes a short-lived connection instead: it sends one `GET /` with `Host: example.com` and `Connection: close`, accepts a response only when the TCP acknowledgement and sequence numbers match, prints only the HTTP status line, and sends RST cleanup afterward. This is an intentionally bounded application-layer probe, not a general HTTP client or Horizon network service.

The physical 0.12.0 run received an initial TCP acknowledgement without payload, reposted the receive correctly, and then parsed `HTTP/1.1 200 OK`. Together with the successful cleanup RST, this is end-to-end evidence that a homebrew process can reach an external HTTP service through the S23 tether. It does **not** create a Horizon network interface or make games/system applications use this transport.

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
