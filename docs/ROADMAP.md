# Roadmap

## Status (2026-10-15)

- [x] Phase 0: audit the prior code and installed libnx headers.
- [x] Phase 1: replace the KIP-oriented build with a clean NRO and stable applet loop.
- [x] Phase 2 (implementation): add a read-only USB interface diagnostic activated with **A**.
- [x] Correct the 0.1.0 console redraw defect observed during the first Switch launch.
- [x] Phase 1/2 physical acceptance: NRO runs; the S23 is absent without tethering and exposes two interfaces with tethering enabled.
- [x] Phase 3 (selection): select RNDIS for the S23 based on its control/data interface signature.
- [x] Phase 3 (implementation): add the user-triggered, reversible RNDIS initialize/response/HALT control probe.
- [x] Phase 3 (physical validation): RNDIS initialization completed successfully on the S23 and HALT was accepted.
- [x] Phase 3 (implementation): add a read-only RNDIS `CURRENT_ADDRESS` query after successful initialization.
- [x] Phase 3 (physical validation): the S23 returned adapter MAC `BA:99:FF:7F:BD:23` after the `CURRENT_ADDRESS` query.
- [x] Phase 4 (implementation): add a no-traffic probe that acquires and opens/closes RNDIS bulk endpoints.
- [x] Phase 4 (physical validation): the S23 RNDIS data interface and both bulk endpoints opened successfully.
- [x] Phase 4 (implementation): add a reversible RNDIS packet-filter enable/clear control probe.
- [x] Phase 4 (physical validation): packet-filter enable and clear completed successfully on the S23.
- [x] Phase 4 (implementation): add a bounded RNDIS bulk-transfer DHCPDISCOVER probe.
- [x] Phase 4 (physical validation): inbound bulk data begins with an RNDIS packet header, proving a non-empty data-plane receive.
- [x] Phase 4 (physical validation): identified the first inbound Ethernet frame as IPv6, proving end-to-end RNDIS frame delivery.
- [x] Phase 4 (physical validation): received a valid DHCPv4 DHCPOFFER after unrelated IPv6 frames.
- [x] Phase 5 (implementation): add DHCPREQUEST/DHCPACK validation without yet applying the lease to a Switch network stack.
- [x] Phase 5 (physical validation): receive DHCPACK for the selected S23 offer; do not configure the lease on Horizon.
- [x] Phase 6 (implementation): parse DHCPACK mask/router/DNS/lease parameters and send a bounded ARP probe for the DHCP router.
- [x] Phase 6 (physical validation): receive the matching ARP reply for the DHCP router.
- [x] Phase 7 (implementation): send one validated ICMP echo request to the resolved DHCP router.
- [x] Phase 7 (physical validation): receive the matching ICMP echo reply from the DHCP router.
- [x] Phase 8 (implementation): send and parse one UDP/DNS A query for `example.com` using the DHCP DNS server.
- [x] Phase 8 (physical validation): receive and parse the matching DNS response.
- [x] Phase 9 (implementation): send one TCP SYN to the first DNS A record, validate SYN-ACK, then send RST cleanup without application data.
- [x] Phase 9 (physical validation): receive the matching external TCP SYN-ACK.
- [x] Phase 10 (implementation): send one HTTP `GET /` after the validated handshake and parse its status line.
- [x] Phase 10 (physical validation): receive and parse a matching HTTP response.
- [ ] Phase 11: assess whether to adopt an embedded network stack rather than extend bespoke protocol probes.
- [ ] Phase 7: only after successful homebrew networking, investigate a constrained Atmosphere sysmodule prototype.

## Acceptance criteria for the current artifact

1. `make` creates `nx-tethering.nro` without warnings/errors.
2. The NRO opens from hbmenu, draws its startup message, and remains open until **PLUS**.
3. Pressing **A** never terminates the app: it reports interfaces or a hexadecimal libnx/Horizon result.
4. A physical Android test captures the descriptor data needed to select a transport. Compilation is not proof of this criterion.

## Risk register

| Risk | Mitigation / evidence needed |
| --- | --- |
| Previous build was a KIP, not a launchable homebrew | NRO-only `switch_rules` build; no `config.json` or `elf2kip`. |
| USB-C role negotiation may not put Switch in host / Android in gadget mode | Record results with the actual cable and Android tethering enabled. |
| `usb:hs` may be unavailable or interface occupied | Show the actual Result code; do not claim access. |
| Android descriptor/protocol is device and firmware dependent | Choose protocol only after the S23 experiment. |
| Horizon has no documented public registration path for a NIC | Keep scope to homebrew transport; defer system-wide claim to Phase 7. |
