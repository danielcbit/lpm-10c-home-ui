# Changelog

Each release is a patch for the official FNIRSI LPM-10C **V2.0.7** TX update
(`LPM-10C_TX_V2.0.7_260907.bin`). The release workflow publishes the section
whose heading matches the tag.

## Unreleased

**New home items:** VLAN test and Duplicate IP (the list now has 12 tools;
neither replaces an existing one).
- VLAN test: the IPv4 Link status checks with every frame 802.1Q-tagged with
  a chosen VLAN (1-4094), so a trunk port can be checked VLAN by VLAN.
- Duplicate IP: ARP probes (RFC 5227) for the DHCP address and for the
  gateway; lists every MAC that conflicts.

**Link status**
- Opens waiting for M instead of running the checks at once, so the cable can
  be plugged in first.

**Switch port** (a second Link status tab: RIGHT shows it, LEFT returns)
- Listens for LLDP and CDP and shows the switch's name and model, the port, the
  port and voice VLANs, any 802.1Q tags seen, the management address, and the
  PoE class and allocated power.
- Sends an LLDP-MED announcement first, so LLDP-MED switches answer at once.
- Sends EAPOL-Start and reports whether the port requires 802.1X.

**Ping monitor** (replaces the vendor PING screen)
- One ping a second with the latest round trip, loss, a graph of the last 190
  round trips, and min, average, max and variation (mean change between
  replies).
- M starts and stops a run; it stops by itself with NO LINK when the cable is
  pulled, and Auto Off waits while it runs.
- Targets: the gateway, 1.1.1.1, 8.8.8.8, or an address typed in its own
  editor (hold M; kept until power off, never written to flash), chosen with
  LEFT/RIGHT. Each run gets its own address by DHCP.

**Theme for the vendor tools**
- Every vendor screen uses the home screen's colours and the Terminus font,
  with titles from the home list ("Ping monitor", "IP scan", ...). Layouts and
  behaviour are unchanged.

## v1.0.0

First public release of the `home-ui` firmware patch.

**New home screen**
- One scrolling list of the ten tools, each with an icon and a plain name
  (Cable map, Cable length, Cable trace, Port flash, PoE voltage, Link status,
  Link speed, Ping monitor, IP scan, Settings) and a position counter,
  replacing the vendor's three pages of 2×2 grid.
- English only, drawn with Terminus 6×12 and 8×16 (SIL OFL 1.1), which are
  crisper on the 240×320 panel than the vendor fonts. The Chinese pictures left
  in English screens (reset warning title, company name in About) are gone.
- A speedometer icon for Link speed, so it no longer shares the Link status icon.

**Link status** (new, in the slot of the vendor QC test)
- IPv4: physical link with speed and duplex, DHCP lease (WARN for an OFFER
  without an ACK), gateway ARP, gateway ping, Internet ping (1.1.1.1, then
  8.8.8.8), and a DNS lookup of `example.com` through the DHCP DNS server with
  1.1.1.1 as fallback, showing the resolved address and the server used.
- IPv6: router advertisement, SLAAC address, DHCPv6 address (SOLICIT,
  ADVERTISE, REQUEST, REPLY, or rapid commit), gateway ping and Internet ping
  (Cloudflare, then Google), with duplicate address detection and answers to
  neighbour solicitations.
- A scrolling list (UP/DOWN) that follows the check in progress.
- The vendor's green arrow marks the port to plug into (top left).

**Fixes**
- **Flash wear:** the vendor firmware rewrote its settings page in flash at
  every power-off. It is now written only when a setting actually changed.
- The battery icon no longer flashes while scrolling the home screen.

**Space**
- 139 KB of vendor pictures and code that nothing reaches any more are cleared
  for reuse (checked by `scripts/check_dead_code.py`); about 119 KB is free.

The patched image has the same size as the vendor file (434,176 bytes): larger
files install only partly on this bootloader.
