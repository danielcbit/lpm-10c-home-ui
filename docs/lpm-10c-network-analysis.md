# LPM-10C networking: static firmware evidence

Inspected 2026-10-19. Target: vendor **TX V2.0.7**, SHA-256 `3bcc50065e16e93ddeff5fb9dce04f1b0447fd3f68a084803070978aef2bcaaa`. This is not an extraction or runtime test of the owner's V2.0.3 installation. Original files and device were unchanged.

## What the binary establishes

All locations below are **file offsets** in the TX BIN. The inferred flash address is offset + `0x08009000`. Focused disassembly is reproduced with `python3 scripts/inspect_network.py` (Python 3 and Capstone 5). The linear decoder includes literal pools and embedded log text; those regions are data, not executable instructions.

| Finding | Evidence | Confidence / limit |
| --- | --- | --- |
| DAVICOM DM9051 Ethernet driver | `Network chip: DAVICOM DM9051` at `0xc634`; `DM9051 id invalid` / `DM9051 id` at `0x1977f` / `0x197a4`; surrounding driver initialization checks identity and link | High static confidence; board marking `DM9051AN` confirmed |
| uIP-derived ARP implementation | `uip_arp_arpin` at `0x1aef7`; routine beginning `0x1ad44` checks 42-byte frames, handles ARP opcodes 1/2, swaps sender/target fields, writes EtherType ARP; routine `0x1af64` resolves subnet/gateway and constructs broadcasts | High for uIP ancestry of ARP; **does not prove a complete stock uIP TCP/IP stack** or its version |
| IPv4 ICMP echo receive | `0x143ba` checks EtherType `0x0800`; `0x14442–0x14456` tests frame byte +23 (IPv4 protocol) == 1 and byte +34 (ICMP type with fixed 20-byte IPv4 header) == 0 | High for implemented ICMP echo-reply path; robustness and packet validation not fully audited |
| ARP host discovery | `0x144d8` checks EtherType `0x0806`; `0x144f4` checks ARP opcode 2; compares target MAC and sender IP before recording result; `ARPScan: Probe` / `Found` logs at `0x429b` / `0x1473b` | High; this scan detects reachable IPv4 hosts via ARP, not advertised application services |
| Raw UDP DHCP client | Routine `0x16e38` builds broadcast Ethernet/IPv4 packet, sets protocol 17 at `0x16e82`, UDP source/destination 68/67 at `0x16eb8–0x16eca`, BOOTP request, DHCP message-type option 53, requested-address/server options 50/54 | High; direct packet construction, not evidence of sockets |
| DHCP receive and options | `0x16fd4` checks IPv4, UDP, client destination port 68, BOOTP reply, transaction ID and MAC; parser `0x16d86` handles options 1, 3, 53, 54 (mask, router, message type, server ID) | High for these fields; no DNS option 6 parser in this helper, although discover requests it. The wait `0x0801FFD4` leaves the matching reply in the frame buffer, which is how the patched Link status reads option 6 |
| DHCP discovery / request orchestration | `0x18a60`: sends message type 1, waits for type 2; sends type 3, waits for type 5; timeout logs at `0x18e30` describe using offered IP without ACK | High static confidence; an OFFER-only address is not a confirmed lease |
| Passive subnet / gateway guessing | Logs `0x18863–0x18958` identify passive ARP sniff, sender subnet votes and gateway hints; fallback warning at `0x692d8` | High for implementation intent; only a bounded code inspection was performed |

No supported identification of lwIP, a BSD socket API, DNS resolver, TCP connection engine, TLS, IPv6, LLDP, mDNS, or HTTP emerged from this focused pass. (The vendor stack has no IPv6: the patched Link status builds its own ICMPv6, neighbour discovery and DHCPv6 packets, and opens the DM9051 multicast filter, register 5 bit 3, while it runs; the vendor setup at `0x080225D2` passes broadcast only.) These are **unverified**, not proven absent. The observed implementation is best described as **a small IPv4/ARP packet engine using uIP-derived ARP, a DM9051 driver, and custom DHCP/diagnostic logic**.

## Useful extensions, ordered by fit

| Extension | Work / reuse | Practical limitation |
| --- | --- | --- |
| Ping history with loss, min/mean/max RTT and variation | Best first feature: existing echo and timing paths plus a bounded sample ring | Validate reply identity/sequence and handle timeouts correctly |
| Gateway / LAN health checklist | Reuse link, ARP, DHCP and ping; show link → address → gateway reachability | Internet or DNS status needs separate tests; ping failure alone is ambiguous |
| DHCP inspector | Show offer, server, subnet, router, timing, ACK status; add lease/DNS option parsing | Fix OFFER-only fallback presentation and lease handling before treating configuration as valid |
| Duplicate IPv4 warning | Reuse ARP probes; show conflicting MAC and address | ARP silence is not proof that an address is free |
| Better host scan | List IP/MAC and live state, configurable range, progress and cancel | Host names require new DNS/mDNS/NBNS logic; MAC vendor names need a data table |
| LLDP/CDP switch-port discovery | Add raw EtherType/LLC handlers and multicast filter configuration | Verify DM9051 receive filtering and actual switch advertisements; no existing parser proven |
| DNS test and resolver latency | Add bounded UDP query/response code and parse DHCP DNS option | New protocol implementation; handle compressed names and malformed replies safely |
| Traceroute | Extend ICMP for TTL and time-exceeded parsing | Routers may filter replies; no existing implementation proven |
| TCP port/service reachability | Requires identifying/enabling or adding a TCP engine | A UDP DHCP client does not establish reusable TCP support |
| HTTP export or small local status page | TCP + HTTP + report representation | Consider after memory, concurrency, update/recovery and security design are understood |

A simple latency strip chart, timeout markers, scan progress bar, and DHCP timeline require little retained data and fit this software evidence well. No throughput, Wi-Fi RF, gigabit certification, packet-capture capacity, or TLS performance claim follows from the existing ping feature. Do not equate Ethernet link rate with achievable software throughput.

## Unresolved implementation questions

The exact MCU suffix, SRAM/flash headroom, SPI clock, packet-buffer ownership, scheduler constraints, full IP validation, TCP build configuration, receive filters, and bootloader acceptance of modified images remain unverified. Static evidence shows several routines sharing packet globals; avoid concurrent networking screens until ownership is mapped. Establish recovery and retain calibration/settings before any future firmware write.

REA's previously saved ELF overview found only one procedure; the prior report documents the raw BIN loader and Linux-only firmware integration limits. This pass reused that limitation and used local Capstone byte evidence instead of repeating broad provider diagnostics. [Existing investigation](lpm-10c-investigation.md) retains artifact identities and earlier REA evidence IDs.
