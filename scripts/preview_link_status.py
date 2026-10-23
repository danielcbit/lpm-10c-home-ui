#!/usr/bin/env python3
"""Run the patched Link status screen offline, against a simulated network.

Loads a built home-ui image and an SRAM snapshot (tests/fixtures/sram-sanitized.bin
by default), like preview_menu.py, and
runs the added code unmodified on a Cortex-M4 emulator. The vendor network
routines it calls are replaced by a model, so each scenario can be checked:
  - dm9051_read, wait_link: link state, speed and duplex
  - dhcp_send, dhcp_wait: an OFFER and an ACK, or not, each left in the frame
    buffer as a DHCP reply whose options may name a DNS server
  - arp_request, arp_wait: the gateway's reply, left in the frame buffer
  - transmit, receive: the gateway checks the IP and ICMP checksums of each echo
    request before it answers, and may first ask for our MAC by ARP; echoes to
    1.1.1.1 and 8.8.8.8 go through it and are answered 12 ms later, or not;
    DNS queries are checked (headers, ports, example.com A IN) and answered by
    the DHCP DNS server (3 ms; on the gateway, or a local host found by ARP) or
    by 1.1.1.1 (15 ms), each with an answer, an error code, or nothing
  - IPv6: a router answers a router solicitation with an advertisement (a
    prefix for SLAAC, the M flag, a lifetime), and its echo requests; before
    answering an echo to our global address it asks for our MAC with a
    neighbour solicitation, which must be answered. A DHCPv6 server may lease
    (ADVERTISE then REPLY, or REPLY at once with rapid commit), advertise only,
    or refuse. Every IPv6 packet's checksum, hop limit and Ethernet address are
    checked, and another host may claim our SLAAC address
  - dm9051_read and dm9051_write of the receive control (register 5): multicast
    frames (router advertisements, neighbour solicitations) reach receive only
    while it passes all multicast, and it must be restored afterwards
  - tick, delay and the DWT cycle counter follow one simulated clock
set_sysState, post_message and the battery icon are recorded, not run.

For every scenario it opens the screen (lpm_link_open, as M on the home item),
checks that nothing ran, presses M (lpm_link_run, event 0x0B), draws it the way the GUI task would (lpm_tool_frame for message 0x36, then
lpm_link_draw for each 0x0C), checks the results, and writes two PNGs: the top
of the list (IPv4) and its end (IPv6). The frame runs the vendor arrow routine
for real, and the green arrow must mark the top-left port and no other. It also checks that the run leaves the
device IP settings (0x20000306) and the Cust_Info record (0x20002CF0) untouched,
and clears the network busy flag, and that UP and DOWN (key events 2 and 3,
through lpm_home_key) scroll the list without touching the menu selection.

The Switch port tab (switch_port.c) has scenarios of its own: RIGHT selects it,
M listens, and the model plays a switch that announces itself by LLDP (after
our LLDP-MED fast start) or by CDP, tags frames with 802.1Q, answers the
EAPOL-Start with an EAP Request/Identity, or stays silent for the full 32 s.
Each frame we send (LLDP, EAPOL-Start) is checked, and the rows and their
details compared with what the switch announced.

  .venv/bin/python scripts/preview_link_status.py     # -> output/link-status/

Needs unicorn in .venv, and clang and lld to find the added code's symbols
(see README). Nothing is sent to the tester.
"""
import argparse
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import preview_menu as P  # noqa: E402

ROOT = P.ROOT
LINK = 0x20020000  # link_state in link_status.c
FRAME, FRAME_LEN, OWN_MAC, NET_BUSY, SYS_STATE = 0x2000F0AC, 0x20000224, 0x20000214, 0x200000A9, 0x20000150
MENU_SELECTION = 0x20000151
TOOLS, TOOLS_MAGIC, TAGGING_MAGIC = 0x20020580, 0x4C504D54, 0x564C414E  # tools_state in tools.h
IP_CONFIG, CUST_INFO = (0x20000306, 0x30), (0x20002CF0, 0xC8)
CYCCNT, DEMCR = 0xE0001004, 0xE000EDFC
OPEN_SITE, FRAME_SITE, DRAW_SITE, RUN_SITE = 0x4830, 0x6C20, 0x6ABE, 0x48CC
KEY_UP, KEY_DOWN = 2, 3
STEPS = ['--', 'TESTING', 'PASS', 'WARN', 'FAIL', 'SKIPPED', 'NONE']
OUR_IP, GATEWAY, MASK, SERVER = bytes([192, 168, 1, 23]), bytes([192, 168, 1, 1]), bytes([255, 255, 255, 0]), \
    bytes([192, 168, 1, 1])
GATEWAY_MAC = bytes.fromhex('001a2b3c0001')
CLOUDFLARE, GOOGLE = bytes([1, 1, 1, 1]), bytes([8, 8, 8, 8])
LOCAL_DNS, LOCAL_DNS_MAC = bytes([192, 168, 1, 53]), bytes.fromhex('001a2b3c0035')
INTERNET_RTT_US = 12000
DNS_RTT_US = {GATEWAY: 3000, LOCAL_DNS: 3000, CLOUDFLARE: 15000}
QUESTION = b'\x07example\x03com\x00\x00\x01\x00\x01'
EXAMPLE_COM = bytes([93, 184, 215, 14])
VENDOR_RCR, RCR_ALL = 0x31, 0x08
GREEN, VENDOR_PANEL = 0x5E4F, 0x2167  # the vendor arrow, on and off, through the theme: LPM_C_PASS, LPM_C_FIELD
BACKGROUND = 0x10C4  # LPM_C_BG
# IPv6: the router is also the DHCPv6 server; another host may claim our SLAAC address.
ROUTER_LL = bytes.fromhex('fe80000000000000021a2bfffe3c0001')
PREFIX = bytes.fromhex('20010db8000100020000000000000000')
LEASED = bytes.fromhex('20010db8000100020000000000001000')
SERVER_DUID = bytes.fromhex('000100012a3b4c5d001a2b3c0001')
OTHER_MAC = bytes.fromhex('001a2b3c0099')
CLOUDFLARE6 = bytes.fromhex('26064700470000000000000000001111')
GOOGLE6 = bytes.fromhex('20014860486000000000000000008888')
ALL_NODES, ALL_ROUTERS = bytes.fromhex('ff020000000000000000000000000001'), bytes.fromhex('ff020000000000000000000000000002')
DHCP6_SERVERS = bytes.fromhex('ff020000000000000000000000010002')
INTERNET6_RTT_US = 20000
IPV4_ROWS, IPV6_ROWS = range(6), range(7, 12)
# link_state in link_status.c (its _Static_assert pins the layout): five RTTs, the router lifetime, steps,
# 15 flag bytes, the IPv4 addresses, two MACs, the IPv6 addresses, the DHCPv6 xid and server DUID.
STATE = struct.Struct('<5IH12s15B4s4s4s4s4s4s6s6s16s16s16s16s16s3s32s')
TOP = 5 * 4 + 2 + 12 + 12  # offset of top, the first row shown

V6_PASS = ['PASS', 'PASS', 'NONE', 'PASS', 'PASS']  # SLAAC, no DHCPv6 server, the usual home network
V6_SKIPPED = ['SKIPPED'] * 5
SCENARIOS = {  # name -> (network model settings, expected steps without the heading, expected detail fragments)
    'all-pass': (dict(), ['PASS'] * 6 + V6_PASS,
                 ['100 Mb/s full', '192.168.1.23 DHCP lease', '00:1A:2B:3C:00:01', 'RTT 0.5 ms',
                  '1.1.1.1 RTT 12.1 ms', 'example.com = 93.184.215.14', 'via 192.168.1.1 (DHCP), 3.1 ms',
                  'fe80::21a:2bff:fe3c:1', 'Prefix 2001:db8:1:2::/64', 'No server; the router does not ask', 'for one',
                  '2606:4700:4700::1111 RTT 20.5 ms']),
    'gateway-asks-arp-first': (dict(arp_first=True), ['PASS'] * 6 + V6_PASS, ['RTT']),
    'no-link': (dict(link=False), ['FAIL'] + ['SKIPPED'] * 5 + V6_SKIPPED, ['No link detected']),
    '10-half': (dict(ten=True, full=False), ['PASS'] * 6 + V6_PASS, ['10 Mb/s half duplex']),
    'no-dhcp': (dict(offer=False), ['PASS', 'FAIL'] + ['SKIPPED'] * 4 + V6_PASS, ['No address obtained']),
    'offer-only': (dict(ack=False), ['PASS', 'WARN', 'PASS', 'PASS', 'PASS', 'PASS'] + V6_PASS,
                   ['OFFER only, no lease', 'via 192.168.1.1 (DHCP)']),
    'no-gateway': (dict(gateway=bytes(4)), ['PASS', 'PASS', 'FAIL'] + ['SKIPPED'] * 3 + V6_PASS,
                   ['No gateway known']),
    'no-arp-reply': (dict(arp=False), ['PASS', 'PASS', 'FAIL'] + ['SKIPPED'] * 3 + V6_PASS,
                     ['192.168.1.1 no ARP reply']),
    'gateway-ignores-ping': (dict(echo=False), ['PASS', 'PASS', 'PASS', 'FAIL', 'PASS', 'PASS'] + V6_PASS,
                             ['No echo reply (may be filtered)', '1.1.1.1 RTT']),
    'only-google-answers': (dict(internet=(GOOGLE,)), ['PASS'] * 6 + V6_PASS, ['8.8.8.8 RTT 12.1 ms']),
    'no-internet': (dict(internet=(), dns_answers=(), internet6=()),
                    ['PASS', 'PASS', 'PASS', 'PASS', 'FAIL', 'FAIL'] + V6_PASS[:4] + ['FAIL'],
                    ['No reply from 1.1.1.1 or 8.8.8.8', 'example.com: no answer', 'Asked 192.168.1.1, 1.1.1.1',
                     'No reply from Cloudflare or Google']),
    'nothing-answers-ping': (dict(echo=False, internet=(), echo6=False, internet6=()),
                             ['PASS', 'PASS', 'PASS', 'FAIL', 'FAIL', 'PASS'] + V6_PASS[:3] + ['FAIL', 'FAIL'],
                             ['No echo reply', 'No reply from', 'via 192.168.1.1']),
    'dhcp-dns-servfail': (dict(dns_rcode={GATEWAY: 2}), ['PASS'] * 5 + ['WARN'] + V6_PASS,
                          ['via 1.1.1.1, 15.1 ms, DHCP DNS failed']),
    'no-dhcp-dns': (dict(dhcp_dns=None), ['PASS'] * 5 + ['WARN'] + V6_PASS, ['via 1.1.1.1, 15.1 ms, no DHCP DNS']),
    'no-dhcp-dns-no-answer': (dict(dhcp_dns=None, dns_answers=()), ['PASS'] * 5 + ['FAIL'] + V6_PASS,
                              ['No DHCP DNS, asked 1.1.1.1']),
    'local-dns-server': (dict(dhcp_dns=LOCAL_DNS), ['PASS'] * 6 + V6_PASS, ['via 192.168.1.53 (DHCP), 3.1 ms']),
    'dhcp-dns-is-1.1.1.1': (dict(dhcp_dns=CLOUDFLARE, dns_answers=()), ['PASS'] * 5 + ['FAIL'] + V6_PASS,
                            ['Asked 1.1.1.1']),
    'dns-cname-first': (dict(cname=True), ['PASS'] * 6 + V6_PASS, ['example.com = 93.184.215.14']),
    'no-ipv6': (dict(ra=False), ['PASS'] * 6 + ['FAIL', 'SKIPPED', 'NONE', 'SKIPPED', 'SKIPPED'],
                ['No router advertisement']),
    'dhcpv6-lease': (dict(managed=True, dhcp6='lease'), ['PASS'] * 6 + ['PASS', 'PASS', 'PASS', 'PASS', 'PASS'],
                     ['2001:db8:1:2::1000', 'DHCPv6 lease']),
    'dhcpv6-rapid-commit': (dict(managed=True, dhcp6='rapid'), ['PASS'] * 6 + ['PASS'] * 5, ['DHCPv6 lease']),
    'dhcpv6-advertise-only': (dict(managed=True, dhcp6='advertise'),
                              ['PASS'] * 6 + ['PASS', 'PASS', 'WARN', 'PASS', 'PASS'], ['ADVERTISE only, no lease']),
    'dhcpv6-no-address': (dict(managed=True, dhcp6='refuse'), ['PASS'] * 6 + ['PASS', 'PASS', 'WARN', 'PASS', 'PASS'],
                          ['The server offers no address']),
    'dhcpv6-missing': (dict(managed=True), ['PASS'] * 6 + ['PASS', 'PASS', 'FAIL', 'PASS', 'PASS'],
                       ['No reply, though the router asks for', 'DHCPv6 (M flag)']),
    'dhcpv6-only': (dict(managed=True, dhcp6='lease', autonomous=False),
                    ['PASS'] * 6 + ['PASS', 'NONE', 'PASS', 'PASS', 'PASS'],
                    ['The router offers no SLAAC prefix', '2606:4700:4700::1111 RTT']),
    'ra-lifetime-0': (dict(lifetime=0), ['PASS'] * 6 + ['WARN', 'PASS', 'NONE', 'PASS', 'SKIPPED'],
                      ['Lifetime 0: not a default router']),
    'slaac-address-in-use': (dict(slaac_in_use=True), ['PASS'] * 6 + ['PASS', 'FAIL', 'NONE', 'PASS', 'SKIPPED'],
                             ['In use by another host (DAD)', 'No global address']),
    'only-google6-answers': (dict(internet6=(GOOGLE6,)), ['PASS'] * 6 + V6_PASS, ['2001:4860:4860::8888 RTT']),
    'ipv6-only': (dict(offer=False, managed=True, dhcp6='rapid'),
                  ['PASS', 'FAIL'] + ['SKIPPED'] * 4 + ['PASS'] * 5, ['No address obtained', 'DHCPv6 lease']),
}


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    total = sum(struct.unpack(f'>{len(data) // 2}H', data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return total


def sealed(data, at):
    """data with the ones' complement checksum (RFC 1071) written at offset at."""
    data = bytearray(data)
    data[at:at + 2] = b'\0\0'
    data[at:at + 2] = struct.pack('>H', 0xFFFF - checksum(bytes(data)))
    return bytes(data)


def pseudo(source, target, payload, next_header):
    return source + target + struct.pack('>IxxxB', len(payload), next_header)


def ipv6_packet(source, target, next_header, payload, hops=255):
    """An IPv6 header and payload, with the ICMPv6 or UDP checksum filled in."""
    at = 6 if next_header == 17 else 2
    body = sealed(pseudo(source, target, payload, next_header) + payload, 40 + at)[40:]
    return bytes([0x60, 0, 0, 0]) + struct.pack('>HBB', len(body), next_header, hops) + source + target + body


def group_mac(ip):
    return b'\x33\x33' + ip[12:]


def eui64(mac):
    return bytes([mac[0] ^ 2]) + mac[1:3] + b'\xff\xfe' + mac[3:]


def solicited_node(ip):
    return bytes.fromhex('ff0200000000000000000001ff') + ip[13:]


class Network:
    """The vendor DM9051, DHCP and ARP routines, as seen by link_status.c."""

    def __init__(self, tester, link=True, ten=False, full=True, offer=True, ack=True, gateway=GATEWAY, arp=True,
                 echo=True, arp_first=False, internet=(CLOUDFLARE, GOOGLE), dhcp_dns=GATEWAY,
                 dns_answers=(GATEWAY, LOCAL_DNS, CLOUDFLARE), dns_rcode=None, cname=False,
                 ra=True, lifetime=1800, managed=False, autonomous=True, dhcp6=None, echo6=True,
                 internet6=(CLOUDFLARE6, GOOGLE6), slaac_in_use=False, lldp=None, cdp=None, cdp_at_ms=None,
                 dot1x=False, tagged=(), ping_rtts=None, keys=(), link_down_at=None, routed=(),
                 trunk=None, conflict_mac=None, gateway_twin=None):
        self.t, self.uc = tester, tester.uc
        self.link, self.ten, self.full, self.offer, self.ack = link, ten, full, offer, ack
        self.gateway, self.arp, self.echo, self.arp_first = gateway, arp, echo, arp_first
        self.internet = internet
        self.dhcp_dns, self.dns_answers, self.dns_rcode, self.cname = dhcp_dns, dns_answers, dns_rcode or {}, cname
        self.ra, self.lifetime, self.managed, self.autonomous = ra, lifetime, managed, autonomous
        self.dhcp6, self.echo6, self.internet6, self.slaac_in_use = dhcp6, echo6, internet6, slaac_in_use
        self.echo_targets = []
        self.dns_queries = []  # servers asked, in order
        self.us = 0
        self.queue = []  # (time available in us, frame)
        self.errors = []
        self.arp_answered = False
        self.echo_requests = 0
        self.rcr, self.rcr_writes = VENDOR_RCR, []
        self.probes = []  # duplicate address detection: the addresses probed, in order
        self.solicitations = 0
        self.dhcp6_messages = []  # DHCPv6 message types received
        self.echo6_targets = []
        self.resolved = set()  # our IPv6 addresses the router has a MAC for (by our neighbour advertisement)
        self.held = []  # echo replies waiting for that
        self.multicast_dropped = 0
        # Switch port: the frames the switch sends, and those we sent it.
        self.lldp, self.cdp, self.dot1x, self.tagged = lldp, cdp, dot1x, tagged
        self.lldp_sent, self.eapol_starts = [], []
        # Ping monitor: the round trip of each echo in turn (None: no reply), and the keys pressed, as
        # (time in us, key, press) items of the key task's queue.
        self.ping_rtts, self.keys = list(ping_rtts) if ping_rtts is not None else None, list(keys)
        self.echo_times = []
        self.link_down_at = link_down_at  # the cable is pulled at this time (us)
        self.routed = (CLOUDFLARE, GOOGLE, *routed)  # addresses beyond the gateway that answer echoes
        # VLAN test: the VLANs the port carries tagged (None: an access port, which drops tagged frames).
        self.trunk = set(trunk or ())
        self.vlans_sent = []  # the VLAN of each tagged frame we sent
        self.dhcp_vlans = []  # the VLAN (0 untagged) of each DHCP DISCOVER or REQUEST
        # Duplicate IP: a host that also uses our address, and a second MAC answering for the gateway.
        self.conflict_mac, self.gateway_twin = conflict_mac, gateway_twin
        self.arp_probes = []
        self.raw_queue = False  # deliver queued frames as they are, without the trunk's tag
        self.sent_frames = []
        self.keys_dropped = 0
        self.idle_resets = 0
        self.app_commands = []
        if cdp is not None:
            self.queue.append((cdp_at_ms * 1000, cdp))
        self.stubs = {0x08016A70: self.dm9051_read, 0x08016AE0: self.dm9051_write, 0x08022088: self.wait_link,
                      0x08022810: self.receive, 0x080228B8: self.transmit, 0x0801FE38: self.dhcp_send,
                      0x0801FFD4: self.dhcp_wait, 0x0802144C: self.arp_request, 0x08021FEC: self.arp_wait,
                      0x08023D38: self.tick, 0x080244E4: self.delay, 0x0800FCC0: self.set_sys_state,
                      0x0800EBE8: lambda: 0, 0x0800EE8C: lambda: 0,
                      # In the vendor arrow 0x0800E408: the FreeRTOS critical section (msr BASEPRI, which the
                      # emulator lacks) and the queue send of its UART log line. The drawing itself runs.
                      0x0802443C: lambda: 0, 0x08024460: lambda: 0, 0x08024828: lambda: 1,
                      0x08024940: self.queue_receive, 0x0800FF04: self.idle_reset,
                      0x08015114: self.app_command}

    def arg(self, n):
        return self.t._arg(n)

    def write(self, address, data):
        if address:
            self.uc.mem_write(address, bytes(data))

    def dm9051_read(self):
        self.us += 5
        reg = self.arg(0)
        if reg == 1:
            ready = any(at <= self.us for at, _ in self.queue)
            return (0x40 if self.linked() else 0) | (0x80 if self.ten else 0) | (1 if ready else 0)
        if reg == 0:
            return 0x08 if self.full else 0
        if reg == 5:
            return self.rcr
        self.errors.append(f'unexpected DM9051 register {reg:#x}')
        return 0

    def dm9051_write(self):
        self.us += 5
        reg, value = self.arg(0), self.arg(1) & 0xFF
        if reg != 5 or value not in (VENDOR_RCR, VENDOR_RCR | RCR_ALL):
            self.errors.append(f'DM9051 register {reg:#x} written with {value:#x}')
        self.rcr = value
        self.rcr_writes.append(value)
        return 0

    def wait_link(self):
        self.us += self.arg(0) * 1000
        return 0

    def tick(self):
        return self.us // 1000

    def delay(self):
        self.us += max(1, self.arg(0)) * 1000
        return 0

    def set_sys_state(self):
        self.write(SYS_STATE, [self.arg(0)])
        self.t.messages.append(0x36)
        return 0

    def tagging(self):
        """The VLAN the wrappers tag with (tools_state in tools.h), or 0."""
        magic, tagging, tag = struct.unpack('<IIH', self.uc.mem_read(TOOLS, 10))
        return tag if tagging == TAGGING_MAGIC else 0

    def tool(self):
        return self.uc.mem_read(TOOLS + 13, 1)[0]

    def carried(self):
        """Whether frames sent now reach the network: untagged always, tagged only on a trunk VLAN."""
        vlan = self.tagging()
        return not vlan or vlan in self.trunk

    def dhcp_send(self):
        self.us += 300
        self.dhcp_vlans.append(self.tagging())
        return 0

    def dhcp_reply(self, kind):
        """The reply dhcp_wait leaves in the frame buffer: BOOTP, cookie and options, with pad bytes."""
        options = bytes([53, 1, kind, 0, 1, 4]) + MASK + bytes([3, 4]) + self.gateway
        if self.dhcp_dns:
            options += bytes([0, 6, 8]) + self.dhcp_dns + GOOGLE  # two servers; the first is used
        options += bytes([54, 4]) + SERVER + b'\xff'
        bootp = bytes([2, 1, 6, 0]) + bytes(12) + OUR_IP + bytes(8) + self.own_mac() + bytes(202) + b'\x63\x82\x53\x63'
        udp = struct.pack('>HHHH', 67, 68, 8 + len(bootp + options), 0) + bootp + options
        ip = bytes([0x45, 0]) + struct.pack('>H', 20 + len(udp)) + bytes(5) + bytes([17]) + bytes(2) + SERVER + OUR_IP
        frame = self.own_mac() + GATEWAY_MAC + b'\x08\x00' + ip + udp
        self.write(FRAME, frame)
        self.write(FRAME_LEN, struct.pack('<H', len(frame)))

    def dhcp_wait(self):
        xid, timeout, kind, ip, mask, gateway, server = (self.arg(n) for n in range(7))
        if self.carried() and ((kind == 2 and self.offer) or (kind == 5 and self.ack)):
            self.us += 3000
            self.dhcp_reply(kind)
            self.write(ip, OUR_IP)
            self.write(mask, MASK)
            if any(self.gateway):
                self.write(gateway, self.gateway)
            self.write(server, SERVER)
            return 1
        self.us += timeout * 1000
        return 0

    def arp_reply(self, delay_us, mac, ip):
        frame = self.own_mac() + mac + b'\x08\x06' + bytes.fromhex('0001080006040002') + mac + ip + self.own_mac() \
            + OUR_IP
        self.queue.append((self.us + delay_us, frame + bytes(60 - len(frame))))

    def arp_request(self):
        target, sender = bytes(self.uc.mem_read(self.arg(0), 4)), bytes(self.uc.mem_read(self.arg(1), 4))
        self.us += 40
        if sender == bytes(4):  # an RFC 5227 probe (Duplicate IP)
            self.arp_probes.append(target)
            if self.conflict_mac and target == OUR_IP:
                self.arp_reply(700, self.conflict_mac, OUR_IP)
            return 0
        if sender != OUR_IP:
            self.errors.append(f'ARP request sent from {sender.hex()}')
        self.asked = target
        if self.tool() == 2 and self.carried():  # Duplicate IP reads the answers itself
            if target == self.gateway and self.arp:
                self.arp_reply(800, GATEWAY_MAC, target)
                if self.gateway_twin:
                    self.arp_reply(1200, self.gateway_twin, target)
        return 0

    def arp_wait(self):
        ip, timeout = bytes(self.uc.mem_read(self.arg(0), 4)), self.arg(1)
        if not self.carried():
            self.us += timeout * 1000
            return 0
        if ip == LOCAL_DNS and self.dhcp_dns == LOCAL_DNS:
            self.write(FRAME, bytes(14) + bytes(8) + LOCAL_DNS_MAC + LOCAL_DNS + bytes(10))
            self.us += 800
            return 1
        if self.arp and ip == self.gateway:
            reply = bytes(14) + bytes(8) + GATEWAY_MAC + self.gateway + bytes(10)
            self.write(FRAME, reply)
            self.us += 800
            return 1
        self.us += timeout * 1000
        return 0

    def own_mac(self):
        return bytes(self.uc.mem_read(OWN_MAC, 6))

    def link_local(self):
        return bytes.fromhex('fe80000000000000') + eui64(self.own_mac())

    def slaac(self):
        return PREFIX[:8] + eui64(self.own_mac())

    def transmit(self):
        self.us += 30
        length = struct.unpack('<H', self.uc.mem_read(FRAME_LEN, 2))[0]
        f = bytes(self.uc.mem_read(FRAME, length))
        self.sent_frames.append(f)
        if f[12:14] == b'\x81\x00':  # 802.1Q: the switch reads the VLAN and passes the frame untagged
            vlan = struct.unpack('>H', f[14:16])[0] & 0xFFF
            self.vlans_sent.append(vlan)
            if vlan != self.tagging():
                self.errors.append(f'frame tagged {vlan} while the wrappers tag {self.tagging()}')
            f, length = f[:12] + f[16:], length - 4
            if vlan not in self.trunk:
                return 0
        elif self.tagging() and f[12:14] not in (b'\x88\xcc', b'\x88\x8e'):
            self.errors.append(f'untagged frame {f[12:14].hex()} during a VLAN test')
        if f[12:14] in (b'\x88\xcc', b'\x88\x8e'):
            return self.transmit_switch(f, length)
        if f[12:14] == b'\x86\xdd':
            return self.transmit6(f, length)
        if f[12:14] == b'\x08\x06':
            if f[20:22] == b'\x00\x02' and f[38:42] == self.gateway and f[28:32] == OUR_IP \
                    and f[22:28] == self.own_mac() and f[0:6] == GATEWAY_MAC and length == 42:
                self.arp_answered = True
            else:
                self.errors.append(f'bad ARP frame {f.hex()}')
            return 0
        ip = f[14:34]
        if ip[9] == 17:
            return self.dns_query(f, length)
        if f[0:6] != GATEWAY_MAC or f[6:12] != self.own_mac() or f[12:14] != b'\x08\x00':
            self.errors.append('echo request has the wrong Ethernet header')
        target = ip[16:20]
        if checksum(ip) != 0xFFFF or ip[9] != 1 or ip[12:16] != OUR_IP or target not in (self.gateway, *self.routed):
            self.errors.append(f'bad IPv4 header {ip.hex()}')
        total = struct.unpack('>H', ip[2:4])[0]
        icmp = f[34:14 + total]
        if checksum(icmp) != 0xFFFF or icmp[0] != 8:
            self.errors.append(f'bad ICMP echo {icmp.hex()}')
        self.echo_requests += 1
        self.echo_targets.append(target)
        self.echo_times.append(self.us)
        if self.arp_first and not self.arp_answered:
            ask = b'\xff' * 6 + GATEWAY_MAC + b'\x08\x06' + bytes.fromhex('000108000604 0001') + GATEWAY_MAC \
                + self.gateway + bytes(6) + OUR_IP
            self.queue.append((self.us + 100, ask))
        if not self.linked():
            answers, delay_us = False, 0
        elif self.ping_rtts:
            delay_us = self.ping_rtts.pop(0)
            answers = delay_us is not None
        elif target == self.gateway:
            answers, delay_us = self.echo and not (self.arp_first and not self.arp_answered
                                                   and self.echo_requests == 1), 400
        else:
            answers, delay_us = target in self.internet, INTERNET_RTT_US
        if answers:
            reply_ip = sealed(ip[:12] + target + OUR_IP, 10)
            reply_icmp = sealed(b'\x00' + icmp[1:], 2)
            self.queue.append((self.us + delay_us, self.own_mac() + GATEWAY_MAC + b'\x08\x00' + reply_ip + reply_icmp))
        return 0

    def transmit_switch(self, f, length):
        """Our LLDP-MED announcement and EAPOL-Start. A switch with LLDP-MED answers the first at once (fast
        start), and its tagged frames follow; an 802.1X port answers the second with an EAP Request/Identity."""
        if length < 60 or f[6:12] != self.own_mac():
            self.errors.append(f'switch frame of {length} bytes from {f[6:12].hex()}')
        if f[12:14] == b'\x88\xcc':
            tlvs = parse_lldp(f[14:length])
            ok = f[:6] == LLDP_GROUP and [t for t, _ in tlvs[:3]] == [1, 2, 3] and tlvs[-1] == (0, b'') \
                and tlvs[0][1] == b'\x04' + self.own_mac() and tlvs[1][1] == b'\x03' + self.own_mac() \
                and (127, bytes.fromhex('0012bb01000101')) in tlvs
            if not ok:
                self.errors.append(f'bad LLDP frame {f[:length].hex()}')
            self.lldp_sent.append(self.us)
            if self.lldp is not None and len(self.lldp_sent) == 1:
                self.queue.append((self.us + 500_000, self.lldp))
                for i, vlan in enumerate(self.tagged):
                    self.queue.append((self.us + 100_000 * (i + 1), tagged_frame(vlan)))
            return 0
        if f[:6] != PAE_GROUP or f[14:18] != b'\x01\x01\x00\x00':
            self.errors.append(f'bad EAPOL-Start {f[:18].hex()}')
        self.eapol_starts.append(self.us)
        if self.dot1x:
            eap = bytes([1, 1, 0, 5, 1])  # code Request, id 1, length 5, type Identity
            request = PAE_GROUP + SWITCH_MAC + b'\x88\x8e' + bytes([2, 0]) + struct.pack('>H', 5) + eap
            self.queue.append((self.us + 20_000, request + bytes(60 - len(request))))
        return 0

    def linked(self):
        return self.link and (self.link_down_at is None or self.us < self.link_down_at)

    def idle_reset(self):
        self.idle_resets += 1
        return 0

    def app_command(self):
        self.app_commands.append(self.arg(0))
        return 0

    def key(self):
        """The next key whose time has come, as the vendor key scan (0x0801630E) queues it: never while the
        network busy flag is set, when the press is lost."""
        if not self.keys or self.keys[0][0] > self.us:
            return None
        _, key, press = self.keys.pop(0)
        if self.uc.mem_read(NET_BUSY, 1)[0]:
            self.keys_dropped += 1
            return None
        return bytes([key, press])

    def queue_receive(self):
        """xQueueReceive(queue, item, ticks) on the key queue: the next key once its time has come. Only
        Ping monitor polls without blocking; anything else would wait forever here."""
        queue, item, ticks = self.arg(0), self.arg(1), self.arg(2)
        if ticks != 0 or queue != struct.unpack('<I', self.uc.mem_read(KEY_QUEUE, 4))[0]:
            self.errors.append(f'xQueueReceive({queue:#x}, ticks {ticks:#x})')
        item_bytes = self.key()
        if item_bytes:
            self.write(item, item_bytes)
            return 1
        return 0

    def dns_query(self, f, length):
        ip, server = f[14:34], f[30:34]
        self.dns_queries.append(server)
        hop = LOCAL_DNS_MAC if server == LOCAL_DNS else GATEWAY_MAC
        if f[0:6] != hop or f[6:12] != self.own_mac() or f[12:14] != b'\x08\x00':
            self.errors.append(f'DNS query to {server.hex()} has the wrong Ethernet header')
        if checksum(ip) != 0xFFFF or ip[12:16] != OUR_IP or struct.unpack('>H', ip[2:4])[0] != length - 14:
            self.errors.append(f'bad IPv4 header {ip.hex()}')
        udp = f[34:length]
        source_port, port, udp_length, _ = struct.unpack('>HHHH', udp[:8])
        query = udp[8:]
        ident, flags, questions = struct.unpack('>HHH', query[:6])
        if port != 53 or udp_length != len(udp) or questions != 1 or query[12:] != QUESTION or not flags & 0x0100:
            self.errors.append(f'bad DNS query {udp.hex()}')
        if server not in self.dns_answers:
            return 0
        rcode = self.dns_rcode.get(server, 0)
        answers = []
        if not rcode:
            if self.cname:  # as a CDN answers: a CNAME, then the A record of its target (by compression)
                target = b'\x03www\xc0\x0c'
                answers.append(b'\xc0\x0c\x00\x05\x00\x01' + struct.pack('>IH', 300, len(target)) + target)
                answers.append(b'\xc0\x29\x00\x01\x00\x01' + struct.pack('>IH', 300, 4) + EXAMPLE_COM)
            else:
                answers.append(b'\xc0\x0c\x00\x01\x00\x01' + struct.pack('>IH', 300, 4) + EXAMPLE_COM)
        response = struct.pack('>HHHHHH', ident, 0x8180 | rcode, 1, len(answers), 0, 0) + QUESTION + b''.join(answers)
        reply_udp = struct.pack('>HHHH', 53, source_port, 8 + len(response), 0) + response
        reply_ip = sealed(bytes([0x45, 0]) + struct.pack('>H', 20 + len(reply_udp)) + bytes(4)
                          + bytes([64, 17, 0, 0]) + server + OUR_IP, 10)
        self.queue.append((self.us + DNS_RTT_US[server], self.own_mac() + hop + b'\x08\x00' + reply_ip + reply_udp))
        return 0

    # ---- IPv6 -------------------------------------------------------------

    def send6(self, delay_us, mac, packet):
        self.queue.append((self.us + delay_us, mac + GATEWAY_MAC + b'\x86\xdd' + packet))

    def advertisement(self):
        flags = 0x80 if self.managed else 0
        options = b'\x01\x01' + GATEWAY_MAC
        options += b'\x05\x01\x00\x00' + struct.pack('>I', 1500)  # MTU, which is not read
        options += bytes([3, 4, 64, 0xC0 if self.autonomous else 0x80]) + struct.pack('>III', 86400, 14400, 0) + PREFIX
        return bytes([134, 0, 0, 0, 64, flags]) + struct.pack('>HII', self.lifetime, 0, 0) + options

    def transmit6(self, f, length):
        ip, mac = f[14:length], f[0:6]
        payload_length, next_header, hops = struct.unpack('>HBB', ip[4:8])
        source, target, payload = ip[8:24], ip[24:40], ip[40:]
        if f[6:12] != self.own_mac() or ip[0] != 0x60 or payload_length != len(payload):
            self.errors.append(f'bad IPv6 header {f[:54].hex()}')
            return 0
        if checksum(pseudo(source, target, payload, next_header) + payload) != 0xFFFF:
            self.errors.append(f'bad IPv6 {next_header} checksum to {target.hex()}')
        if target[0] == 0xFF and mac != group_mac(target) or target[0] != 0xFF and mac != GATEWAY_MAC:
            self.errors.append(f'IPv6 packet to {target.hex()} sent to MAC {mac.hex()}')
        if next_header == 17:
            return self.dhcp6_message(source, target, payload)
        if next_header != 58:
            self.errors.append(f'IPv6 next header {next_header}')
            return 0
        kind = payload[0]
        if kind in (133, 135, 136) and hops != 255:
            self.errors.append(f'neighbour discovery {kind} with hop limit {hops}')
        if kind == 135:  # only duplicate address detection: the router's own solicitations are answered by NA
            probed = payload[8:24]
            if source != bytes(16) or target != solicited_node(probed) or len(payload) != 24:
                self.errors.append(f'bad duplicate address probe {payload.hex()}')
            self.probes.append(probed)
            if self.slaac_in_use and probed == self.slaac():  # the owner defends it, to all nodes
                na = bytes([136, 0, 0, 0, 0x20, 0, 0, 0]) + probed + b'\x02\x01' + OTHER_MAC
                self.queue.append((self.us + 500, group_mac(ALL_NODES) + OTHER_MAC + b'\x86\xdd'
                                   + ipv6_packet(probed, ALL_NODES, 58, na)))
        elif kind == 136:
            answered = payload[8:24]
            if source != answered or target != ROUTER_LL or payload[4] != 0x60 or payload[24:32] != b'\x02\x01' \
                    + self.own_mac():
                self.errors.append(f'bad neighbour advertisement {payload.hex()}')
            self.resolved.add(answered)
            for at, frame in self.held:
                self.queue.append((max(at, self.us + 300), frame))
            self.held = []
        elif kind == 133:
            self.solicitations += 1
            if source != self.link_local() or target != ALL_ROUTERS or payload[8:16] != b'\x01\x01' + self.own_mac():
                self.errors.append(f'bad router solicitation {payload.hex()} from {source.hex()}')
            if self.ra:  # to all nodes, as most routers answer
                self.send6(40000, group_mac(ALL_NODES), ipv6_packet(ROUTER_LL, ALL_NODES, 58, self.advertisement()))
        elif kind == 128:
            self.echo6_targets.append(target)
            internet = target != ROUTER_LL
            expected_source = self.link_local() if not internet else None
            if internet and source not in (self.slaac(), LEASED) or not internet and source != expected_source:
                self.errors.append(f'echo to {target.hex()} from {source.hex()}')
            if hops != 64 or payload[4:6] != b'\x4c\x50':
                self.errors.append(f'bad ICMPv6 echo {payload.hex()}')
            if (target in self.internet6) if internet else self.echo6:
                reply = ipv6_packet(target, source, 58, bytes([129]) + payload[1:], hops=64)
                delay_us = INTERNET6_RTT_US if internet else 600
                frame = self.own_mac() + GATEWAY_MAC + b'\x86\xdd' + reply
                if source in self.resolved or source == self.link_local():  # learnt from the solicitation
                    self.queue.append((self.us + delay_us, frame))
                else:  # the router asks for our MAC first, at our solicited-node group
                    self.held.append((self.us + delay_us, frame))
                    ns = bytes([135, 0, 0, 0, 0, 0, 0, 0]) + source + b'\x01\x01' + GATEWAY_MAC
                    self.send6(200, group_mac(solicited_node(source)),
                               ipv6_packet(ROUTER_LL, solicited_node(source), 58, ns))
        else:
            self.errors.append(f'unexpected ICMPv6 type {kind}')
        return 0

    def dhcp6_message(self, source, target, udp):
        source_port, port, udp_length, _ = struct.unpack('>HHHH', udp[:8])
        message = udp[8:]
        kind, xid = message[0], message[1:4]
        options, at = {}, 4
        while at + 4 <= len(message):
            code, size = struct.unpack('>HH', message[at:at + 4])
            options[code] = message[at + 4:at + 4 + size]
            at += 4 + size
        self.dhcp6_messages.append(kind)
        if source != self.link_local() or target != DHCP6_SERVERS or (source_port, port) != (546, 547) \
                or udp_length != len(udp) or at != len(message):
            self.errors.append(f'bad DHCPv6 datagram {udp.hex()}')
        if options.get(1) != b'\x00\x03\x00\x01' + self.own_mac() or 8 not in options \
                or options.get(3, b'')[:4] != b'\x00\x00\x00\x01':
            self.errors.append(f'bad DHCPv6 options {options}')
        if kind == 1 and 14 not in options or kind == 3 and (options.get(2) != SERVER_DUID
                                                              or options[3][12:16] != b'\x00\x05\x00\x18'
                                                              or options[3][16:32] != LEASED):
            self.errors.append(f'bad DHCPv6 {kind} options {options}')
        if kind not in (1, 3):
            self.errors.append(f'DHCPv6 message type {kind}')
            return 0
        mode = self.dhcp6
        if mode is None or kind == 3 and mode == 'advertise':
            return 0
        address = b'\x00\x05\x00\x18' + LEASED + struct.pack('>II', 3600, 7200)
        ia = b'\x00\x00\x00\x01' + struct.pack('>II', 1800, 2880) + address
        reply_kind = 7 if kind == 3 or mode == 'rapid' else 2
        body = bytes([reply_kind]) + xid + struct.pack('>HH', 1, len(options[1])) + options[1] \
            + struct.pack('>HH', 2, len(SERVER_DUID)) + SERVER_DUID
        if mode == 'refuse':  # NoAddrsAvail
            status = struct.pack('>H', 2) + b'no addresses'
            body += struct.pack('>HH', 13, len(status)) + status
        else:
            body += struct.pack('>HH', 3, len(ia)) + ia
            if reply_kind == 7 and kind == 1:
                body += struct.pack('>HH', 14, 0)
        reply = struct.pack('>HHHH', 547, 546, 8 + len(body), 0) + body
        self.send6(2000, self.own_mac(), ipv6_packet(ROUTER_LL, self.link_local(), 17, reply, hops=64))
        return 0

    def receive(self):
        self.us += 20
        while True:
            ready = [item for item in self.queue if item[0] <= self.us]
            if not ready:
                return 0
            self.queue.remove(ready[0])
            frame = ready[0][1]
            # The DM9051 hash passes only broadcast; other multicast needs "pass all multicast".
            if frame[0] & 1 and frame[:6] != b'\xff' * 6 and not self.rcr & RCR_ALL:
                self.multicast_dropped += 1
                continue
            vlan = self.tagging()
            if vlan and not self.raw_queue:  # the trunk delivers the VLAN's frames tagged
                frame = frame[:12] + struct.pack('>HH', 0x8100, vlan) + frame[12:]
            self.write(FRAME, frame)
            return len(frame)

    def cyccnt(self):
        return (self.us * 144) & 0xFFFFFFFF


class LinkTester(P.Tester):
    def __init__(self, image, sram, text_routine):
        super().__init__(image, sram)
        self.text_routine, self.drawn = text_routine, []
        from unicorn import UC_HOOK_MEM_READ
        self.uc.mem_map(0xE0000000, 0x100000)  # the Cortex-M private bus: DWT and DEMCR are plain memory here
        self.net = None
        self.uc.hook_add(UC_HOOK_MEM_READ, self._cyccnt, begin=CYCCNT, end=CYCCNT + 3)

    def _cyccnt(self, uc, access, address, size, value, _):
        if self.net:
            uc.mem_write(CYCCNT, struct.pack('<I', self.net.cyccnt()))

    def _hook(self, uc, address, size, _):
        from unicorn.arm_const import UC_ARM_REG_R0
        if address == self.text_routine:  # text(font, x, y, s, fg, bg): keep the string
            pointer = self._arg(3)
            raw = bytes(uc.mem_read(pointer, 64))
            self.drawn.append(raw[:raw.index(0)].decode('ascii', 'replace'))
        if self.net and address in self.net.stubs:
            uc.reg_write(UC_ARM_REG_R0, self.net.stubs[address]() & 0xFFFFFFFF)
            self._return()
            return
        super()._hook(uc, address, size, _)


# ---- Switch port ------------------------------------------------------------------------------------------

SWITCH = 0x20020100  # switch_state in switch_port.h (its _Static_assert pins the layout)
SWITCH_STATE = struct.Struct('<7s10B4sxHHH6H32s32s32s32s')
SWITCH_MAC = bytes.fromhex('00a0c9112233')
LLDP_GROUP, PAE_GROUP = bytes.fromhex('0180c200000e'), bytes.fromhex('0180c2000003')
CDP_GROUP = bytes.fromhex('01000ccccccc')
KEY_LEFT, KEY_RIGHT = 4, 5
SWITCH_ROWS = ['Physical link', 'Switch', 'Port', 'VLAN', 'Management', 'PoE', '802.1X']


def tlv(kind, value):
    return struct.pack('>H', kind << 9 | len(value)) + value


def parse_lldp(data):
    out, at = [], 0
    while at + 2 <= len(data):
        head = struct.unpack('>H', data[at:at + 2])[0]
        kind, n = head >> 9, head & 0x1FF
        out.append((kind, data[at + 2:at + 2 + n]))
        at += 2 + n
        if kind == 0:
            break
    return out


def lldp_frame():
    """An access switch port as a Cisco or Aruba LLDP-MED switch announces it: port VLAN 20, voice VLAN 120,
    PoE class 4 with 25.5 W allocated, a management address, and a system description longer than one line."""
    voice = 1 << 22 | 120 << 9 | 5 << 6 | 46  # tagged, VLAN 120, priority 5, DSCP EF
    body = (tlv(1, b'\x04' + SWITCH_MAC) + tlv(2, b'\x05Gi1/0/12') + tlv(3, b'\x00\x78')
            + tlv(4, b'Office 2.14 desk B') + tlv(5, b'core-sw1.example.net')
            + tlv(6, b'Cisco IOS Software, C9300 Software (CAT9K_IOSXE), Version 17.9.4')
            + tlv(8, bytes([5, 1, 10, 0, 0, 2, 2, 0, 0, 0, 1, 0]))
            + tlv(127, bytes.fromhex('0080c201') + struct.pack('>H', 20))
            + tlv(127, bytes.fromhex('00120f02') + bytes([0x0F, 1, 5, 0x51]) + struct.pack('>HH', 255, 255))
            + tlv(127, bytes.fromhex('0012bb0201') + voice.to_bytes(3, 'big'))
            + tlv(0, b''))
    return LLDP_GROUP + SWITCH_MAC + b'\x88\xcc' + body


def cdp_frame():
    """A CDP v2 announcement: device ID, address, port, platform, native VLAN 30, voice VLAN 130."""
    def t(kind, value):
        return struct.pack('>HH', kind, 4 + len(value)) + value
    body = (bytes([2, 180, 0, 0]) + t(1, b'edge-sw7') + t(2, struct.pack('>I', 1) + bytes([1, 1, 0xCC]) +
            struct.pack('>H', 4) + bytes([172, 16, 0, 7])) + t(3, b'FastEthernet0/7') + t(6, b'cisco WS-C2960-24TT-L')
            + t(0x0A, struct.pack('>H', 30)) + t(0x0E, b'\x01' + struct.pack('>H', 130)))
    llc = bytes.fromhex('aaaa0300000c2000') + body
    return CDP_GROUP + SWITCH_MAC + struct.pack('>H', len(llc)) + llc


def tagged_frame(vlan):
    """A broadcast ARP request, tagged with vlan, as a trunk port would pass it."""
    arp = bytes.fromhex('0001080006040001') + OTHER_MAC + bytes([10, 9, 0, 1]) + bytes(6) + bytes([10, 9, 0, 2])
    return b'\xff' * 6 + OTHER_MAC + b'\x81\x00' + struct.pack('>H', vlan) + b'\x08\x06' + arp + bytes(14)


SWITCH_SCENARIOS = {  # name -> (network model settings, expected steps, expected detail fragments)
    'switch-lldp-med': (dict(lldp=lldp_frame(), tagged=(120, 20, 120)),
                        ['PASS', 'PASS', 'PASS', 'PASS', 'PASS', 'PASS', 'NONE'],
                        ['core-sw1.example.net (LLDP)', 'Cisco IOS Software, C9300 Soft', 'Gi1/0/12',
                         'Office 2.14 desk B', 'Port VLAN 20, voice 120', 'Tagged: VLAN 120, 20', '10.0.0.2',
                         'Class 4, 25.5 W', 'Not required (no EAP request)']),
    'switch-cdp-dot1x': (dict(cdp=cdp_frame(), cdp_at_ms=9000, dot1x=True),
                         ['PASS', 'PASS', 'PASS', 'PASS', 'PASS', 'NONE', 'WARN'],
                         ['edge-sw7 (CDP)', 'cisco WS-C2960-24TT-L', 'FastEthernet0/7', 'Port VLAN 30, voice 130',
                          '172.16.0.7', 'Required: the switch asked who we are']),
    'switch-silent': (dict(),
                      ['PASS', 'NONE', 'NONE', 'NONE', 'NONE', 'NONE', 'NONE'],
                      ['No LLDP or CDP in 32 s', 'The switch may not send them', 'Not announced, no tagged frames']),
    'switch-no-link': (dict(link=False),
                       ['FAIL', 'SKIPPED', 'SKIPPED', 'SKIPPED', 'SKIPPED', 'SKIPPED', 'SKIPPED'],
                       ['No link detected']),
}


def read_switch(uc):
    f = SWITCH_STATE.unpack(bytes(uc.mem_read(SWITCH, SWITCH_STATE.size)))
    text = [t[:t.index(0)].decode() if 0 in t else t.decode() for t in f[21:25]]
    return {'steps': [STEPS[s] if s < len(STEPS) else f'?{s}' for s in f[0]], 'tab': f[1], 'drawn_tab': f[2],
            'top': f[3], 'heard': f[4], 'dot1x': f[5], 'address': f[11], 'vlan': f[12], 'voice_vlan': f[13],
            'poe_dw': f[14], 'tagged': list(f[15:15 + f[10]]), 'name': text[0], 'platform': text[1],
            'port': text[2], 'port_desc': text[3]}


def run_switch_scenario(image, sram, symbols, entries, settings, expected, fragments, out, name):
    """Open Link status with the cable unplugged, plug it in, RIGHT to the Switch port tab, M, then draw the
    result, its end and the checklist again after LEFT. Returns the problems."""
    open_, frame, draw, run = entries
    key = symbols['lpm_home_key'] & ~1
    tester = LinkTester(image, sram, symbols['text'] & ~1)
    tester.uc.mem_write(SWITCH, b'\xa5' * SWITCH_STATE.size)  # whatever the RAM held: the open must clear it
    tester.net = Network(tester, link=False)
    tester.call(open_)
    tester.net = net = Network(tester, **settings)
    problems = []
    if read_switch(tester.uc)['tab'] != 0:
        problems.append('Link status did not open on the checklist')
    tester.call(frame)
    tester.messages.clear()
    tester.call(key, KEY_RIGHT)
    if read_switch(tester.uc)['tab'] != 1 or tester.messages != [0x0C]:
        problems.append(f'RIGHT: tab {read_switch(tester.uc)["tab"]}, messages {tester.messages}')
    tester.call(draw)
    tester.drawn.clear()
    tester.call(run)
    state = read_switch(tester.uc)
    tester.call(frame)
    tester.call(draw)
    P.png(out / f'{name}.png', tester.fb, P.W, P.H)
    tester.uc.mem_write(SWITCH + 9, b'\xff')  # scrolled past the end: the draw stops at the last row
    tester.call(draw)
    P.png(out / f'{name}-end.png', tester.fb, P.W, P.H)
    drawn = ' | '.join(tester.drawn)
    problems += list(net.errors)
    if state['steps'] != expected:
        problems.append(f'steps {state["steps"]}, expected {expected}')
    problems += [f'"{text}" not drawn' for text in fragments if text not in drawn]
    if 'Switch port' not in drawn or 'Checklist' not in drawn:
        problems.append('tabs not drawn')
    if tester.uc.mem_read(NET_BUSY, 1)[0] != 0:
        problems.append('network busy flag left set')
    seconds = net.us / 1e6
    if not settings.get('link', True):
        if net.lldp_sent or net.eapol_starts or net.rcr_writes:
            problems.append('frames sent without a link')
    else:
        if net.rcr_writes[:1] != [VENDOR_RCR | RCR_ALL] or net.rcr != VENDOR_RCR:
            problems.append(f'receive control writes {[hex(v) for v in net.rcr_writes]}')
        if len(net.lldp_sent) != 1:
            problems.append(f'{len(net.lldp_sent)} LLDP frames sent, expected one')
        # The listening ends 3 s after the first announcement, else at 32 s; EAPOL-Start goes every 5 s until
        # an EAP request answers it.
        heard_at = 0.5 if net.lldp is not None else (settings.get('cdp_at_ms') or 0) / 1000
        limit = heard_at + 3 if heard_at else 32
        if not limit <= seconds <= limit + 0.1:
            problems.append(f'listened {seconds:.2f} s, expected {limit} s')
        starts = 1 if net.dot1x else 1 + int((limit - 1e-3) // 5)
        if len(net.eapol_starts) != starts:
            problems.append(f'{len(net.eapol_starts)} EAPOL-Starts, expected {starts}')
    tester.messages.clear()
    tester.drawn.clear()
    tester.call(key, KEY_LEFT)
    tester.call(draw)
    if read_switch(tester.uc)['tab'] != 0 or 'Physical link' not in ' | '.join(tester.drawn):
        problems.append('LEFT did not return to the checklist')
    return problems, seconds


# ---- Ping monitor ------------------------------------------------------------------------------------------

PING = 0x20020200  # ping_state in ping_monitor.c (its _Static_assert pins the layout)
PING_STATE = struct.Struct('<190IQQ6IHBBBBBBBBBBB4s6sBB4s')
PING_HISTORY, TIMEOUT = 190, 0xFFFFFFFF
KEY_QUEUE, IP_SETTINGS = 0x20000054, 0x20000306
PING_OPEN_SITE, PING_DRAW_SITE = 0x484E, 0x6B04
PING_PHASES = ['no link', 'no address', 'no gateway', 'ready', 'starting', 'idle', 'link lost']
KEY_MATCH = 0x08016180
ITEM = 0x20001100  # scratch RAM for a key item, as the key task holds it on its stack
# Vendor key items {key, press}: BACK 0, RIGHT 1, DOWN 2, UP 3, M 4, LEFT 5; press 3 short, 6 held.
BACK, RIGHT, DOWN, UP, M, LEFT = 0, 1, 2, 3, 4, 5
SHORT, HELD = 3, 6


def read_ping(uc):
    f = PING_STATE.unpack(bytes(uc.mem_read(PING, PING_STATE.size)))
    rtts, rest = f[:PING_HISTORY], f[PING_HISTORY:]
    head, count = rest[9], rest[10]
    ring = [rtts[(head + PING_HISTORY - count + i) % PING_HISTORY] for i in range(count)]
    return {'ring': ring, 'sum_us': rest[0], 'change_sum_us': rest[1], 'sent': rest[2], 'received': rest[3],
            'min_us': rest[4], 'max_us': rest[5], 'changes': rest[6], 'seq': rest[8],
            'phase': PING_PHASES[rest[11]] if rest[11] < len(PING_PHASES) else rest[11], 'target': rest[12],
            'running': rest[13], 'request': rest[14], 'editing': rest[15], 'target_ip': rest[20],
            'cursor': rest[22], 'edit_ip': rest[24]}


def wave(n):
    """Round trips like the design's: about 4 ms with a spike to 18 ms every 24 pings."""
    return [18_000 - (i % 24) * 3000 if i % 24 < 4 else 3_200 + (i * 7919) % 1700 for i in range(n)]


PING_SCENARIOS = {
    # name -> (network model settings, checks). Every run starts with M; nothing pings before it.
    'ping-gateway': (dict(ping_rtts=[None if i in (20, 47, 75, 101, 118) else r for i, r in enumerate(wave(150))],
                          keys=[(0, M, SHORT), (150.5e6, BACK, SHORT)]),
                     dict(sent=150, lost=5, target=GATEWAY, state=2)),
    'ping-waits-for-m': (dict(keys=[(2e6, BACK, SHORT)]),
                         dict(sent=0, lost=0, phase='idle', state=2, drawn=['Press M to start', 'M start'])),
    'ping-stop-then-target': (dict(ping_rtts=[500] * 5, keys=[(0, M, SHORT), (3.5e6, M, SHORT), (4e6, RIGHT, SHORT),
                                                               (5e6, BACK, SHORT)]),
                              dict(sent=0, lost=0, phase='idle', target_index=1, state=2, running=0, echoes=4,
                                   drawn=['To 1.1.1.1', 'Press M to start'])),
    'ping-change-target': (dict(ping_rtts=[500] * 10 + [12_000] * 6,
                                keys=[(0, M, SHORT), (5.5e6, RIGHT, SHORT), (11.5e6, BACK, SHORT)]),
                           dict(sent=6, lost=0, target=CLOUDFLARE, state=2, targets=[GATEWAY] * 6 + [CLOUDFLARE] * 7)),
    'ping-left-skips-unset-saved': (dict(ping_rtts=[12_000] * 4, keys=[(0, M, SHORT), (1.5e6, LEFT, SHORT),
                                                                       (4.5e6, BACK, SHORT)]),
                                    dict(sent=3, lost=0, target=GOOGLE, state=2)),
    'ping-no-link': (dict(link=False, keys=[(0, M, SHORT), (5e6, BACK, SHORT)]),
                     dict(sent=0, lost=0, phase='no link', state=2, drawn=['No link'])),
    'ping-link-lost': (dict(ping_rtts=[600] * 20, link_down_at=4.5e6, keys=[(0, M, SHORT), (8e6, BACK, SHORT)]),
                       dict(sent=5, lost=0, phase='link lost', state=2, drawn=['NO LINK', 'M start'])),
    # Hold M during a run: it stops and the editor opens on the target, 1.1.1.1. LEFT wraps to the last
    # digit, UP makes it 1.1.1.2, M uses it; M then pings it.
    'ping-set-address': (dict(ping_rtts=[500] * 10, routed=[bytes([1, 1, 1, 2])], keys=[(0, RIGHT, SHORT), (0.5e6, M, SHORT), (2e6, M, HELD),
                                                          (3e6, LEFT, SHORT), (3.5e6, UP, SHORT), (4e6, M, SHORT),
                                                          (4.5e6, M, SHORT), (7.3e6, BACK, SHORT)]),
                         dict(sent=3, lost=0, target=bytes([1, 1, 1, 2]), saved=bytes([1, 1, 1, 2]), state=2,
                              editing=0, drawn=['To 1.1.1.2'])),
    # The editor itself, left open: 192.168.1.1 with the cursor moved to the third octet, which DOWN
    # takes from 1 to 0 and on to 9 (a hundreds digit wraps at 2: 255 at most).
    'ping-address-editor': (dict(keys=[(0, M, SHORT), (2.5e6, M, HELD), (3e6, LEFT, SHORT), (3.2e6, LEFT, SHORT),
                                       (3.4e6, LEFT, SHORT), (3.6e6, LEFT, SHORT), (3.8e6, DOWN, SHORT),
                                       (4e6, DOWN, SHORT)], ping_rtts=[500] * 3),
                            dict(sent=3, lost=0, state=11, editing=1, cursor=8, edit_ip=bytes([192, 168, 9, 1]),
                                 saved=bytes(4), drawn=['TARGET ADDRESS', 'M ok  BACK cancel'])),
    # BACK leaves the editor without changing the target, then goes home.
    'ping-address-cancel': (dict(keys=[(0, M, HELD), (0.5e6, UP, SHORT), (1e6, BACK, SHORT), (1.5e6, BACK, SHORT)]),
                            dict(sent=0, lost=0, state=2, editing=0, saved=bytes(4), target_index=0)),
}


def run_ping_scenario(image, sram, symbols, entries, settings, checks, out, name):
    """Open Ping monitor from the home screen and let it ping until the scripted keys stop it; then draw
    the last screen as the GUI task would. Returns the problems and the simulated seconds."""
    open_, frame, draw = entries
    tester = LinkTester(image, sram, symbols['text'] & ~1)
    for base, size in ((0x40000000, 0x30000), (0x50000000, 0x10000)):  # the key path touches timers and GPIO
        tester.uc.mem_map(base, size)
    tester.uc.mem_write(KEY_QUEUE, struct.pack('<I', 0x20001000))
    tester.uc.mem_write(PING, b'\xa5' * PING_STATE.size)  # whatever the RAM held: the open must clear it
    tester.uc.mem_write(IP_SETTINGS + 0x14, bytes(4))  # no saved PING target
    tester.net = net = Network(tester, **settings)
    before = bytes(tester.uc.mem_read(CUST_INFO[0], CUST_INFO[1]))
    problems = []
    try:
        tester.call(open_)
        if net.echo_targets:
            problems.append('pinged before M')
        tester.call(frame)
        tester.call(draw)
        # The key task: each key not taken by a run's own polling goes through the vendor key table. Each
        # screen is drawn when a key changes it, so what was shown can be checked.
        while net.keys:
            net.us = max(net.us, int(net.keys[0][0]))
            item = net.key()
            if item:
                tester.uc.mem_write(ITEM, item + bytes(2))
                tester.call(KEY_MATCH, ITEM)
                if tester.uc.mem_read(SYS_STATE, 1)[0] == 11:
                    tester.call(draw)
    except SystemExit as error:
        problems.append(f'run: {error}')
    state = read_ping(tester.uc)
    final_state = tester.uc.mem_read(SYS_STATE, 1)[0]
    # The last screen: back in state 11 for the drawing, as it was before BACK.
    tester.uc.mem_write(SYS_STATE, bytes([11]))
    tester.call(frame)
    tester.call(draw)
    P.png(out / f'{name}.png', tester.fb, P.W, P.H)
    problems += list(net.errors)
    if final_state != checks['state']:
        problems.append(f'ended in state {final_state}, expected {checks["state"]}')
    if tester.uc.mem_read(NET_BUSY, 1)[0]:
        problems.append('network busy flag set: the key scan would drop every key')
    if net.keys_dropped:
        problems.append(f'{net.keys_dropped} key presses dropped')
    if 'running' in checks and state['running'] != checks['running']:
        problems.append(f'running {state["running"]}')
    if 'target_index' in checks and state['target'] != checks['target_index']:
        problems.append(f'target index {state["target"]}')
    if state['sent'] and net.idle_resets < state['sent']:
        problems.append(f'Auto Off idle count cleared {net.idle_resets} times in {state["sent"]} pings')
    if state['sent'] != checks['sent'] or state['sent'] - state['received'] != checks['lost']:
        problems.append(f'{state["sent"]} sent, {state["sent"] - state["received"]} lost; expected '
                        f'{checks["sent"]} and {checks["lost"]}')
    if 'phase' in checks and state['phase'] != checks['phase']:
        problems.append(f'phase {state["phase"]}')
    if 'target' in checks and state['target_ip'] != checks['target']:
        problems.append(f'target {state["target_ip"].hex()}')
    for field in ('editing', 'cursor', 'edit_ip'):
        if field in checks and state[field] != checks[field]:
            problems.append(f'{field} {state[field]!r}')
    saved = bytes(tester.uc.mem_read(IP_SETTINGS + 0x14, 4))
    if 'saved' in checks and saved != checks['saved']:
        problems.append(f'saved target {saved.hex()}')
    if 'echoes' in checks and len(net.echo_targets) != checks['echoes']:
        problems.append(f'{len(net.echo_targets)} echoes, expected {checks["echoes"]}: M did not stop the run')
    if 'targets' in checks and net.echo_targets[:len(checks['targets'])] != checks['targets']:
        problems.append(f'echo targets {[t.hex() for t in net.echo_targets]}')
    if bytes(tester.uc.mem_read(CUST_INFO[0], CUST_INFO[1])) != before:
        problems.append('Cust_Info changed')
    # One echo a second: each starts 1 to 1.1 s after the one before, unless a key came between them
    # (a key restarts at once, and the echo it interrupted does not count).
    keys = [k[0] for k in settings.get('keys', ())]
    gaps = [b - a for a, b in zip(net.echo_times, net.echo_times[1:]) if not any(a - 50_000 <= k <= b for k in keys)]
    if gaps and not all(990_000 <= g <= 1_100_000 for g in gaps):
        problems.append(f'echo intervals {sorted(set(g // 1000 for g in gaps))} ms')
    if name == 'ping-gateway':
        replies = [r for r in state['ring'] if r != TIMEOUT]
        sent = [r for r in settings['ping_rtts'] if r is not None]
        expected_ring = [None if r is None else r for r in settings['ping_rtts']][-PING_HISTORY:]
        if len(state['ring']) != len(expected_ring) or any(
                (want is None) != (got == TIMEOUT) or (want is not None and abs(got - want) > 200)
                for want, got in zip(expected_ring, state['ring'])):
            problems.append(f'ring of {len(state["ring"])}, last {state["ring"][-1]}')
        if abs(state['min_us'] - min(sent)) > 200 or abs(state['max_us'] - max(sent)) > 200:
            problems.append(f'min {state["min_us"]}, max {state["max_us"]}')
        mean = state['sum_us'] / state['received']
        if abs(mean - sum(sent) / len(sent)) > 200:
            problems.append(f'mean {mean:.0f} us, expected {sum(sent) / len(sent):.0f}')
        if not replies:
            problems.append('no replies in the ring')
    drawn = ' | '.join(tester.drawn)
    for text in checks.get('drawn', []):
        if text not in drawn:
            problems.append(f'"{text}" not drawn')
    return problems, net.us / 1e6


def check_vlan_wrappers(image, sram, symbols):
    """lpm_vlan_transmit and lpm_vlan_receive, as the vendor DHCP and ARP routines reach them: untouched
    without a VLAN test; during one, the tag goes in before sending and the frame is put back after, and only
    frames with the VLAN's tag come in, untagged."""
    from unicorn.arm_const import UC_ARM_REG_R0
    tester = LinkTester(image, sram, symbols['text'] & ~1)
    tester.net = net = Network(tester, trunk={20})
    net.raw_queue = True
    transmit, receive = symbols['lpm_vlan_transmit'] & ~1, symbols['lpm_vlan_receive'] & ~1
    frame = b'\xff' * 6 + bytes.fromhex('020000000001') + b'\x08\x06' + bytes(range(46))
    tag20 = struct.pack('>HH', 0x8100, 20)
    problems = []

    def send(tagging):
        tester.uc.mem_write(TOOLS, struct.pack('<IIHH', TOOLS_MAGIC, TAGGING_MAGIC if tagging else 0, 20, 20))
        tester.uc.mem_write(FRAME, frame)
        tester.uc.mem_write(FRAME_LEN, struct.pack('<H', len(frame)))
        tester.call(transmit)
        after = bytes(tester.uc.mem_read(FRAME, len(frame))), struct.unpack('<H', tester.uc.mem_read(FRAME_LEN, 2))[0]
        return net.sent_frames[-1], after

    sent, after = send(False)
    if sent != frame:
        problems.append('untagged transmit changed the frame')
    sent, after = send(True)
    if sent != frame[:12] + tag20 + frame[12:]:
        problems.append(f'tagged transmit sent {sent[:18].hex()}')
    if after != (frame, len(frame)):
        problems.append('the frame was not put back after a tagged transmit')
    for label, arriving, expected in (('VLAN 20', frame[:12] + tag20 + frame[12:], frame),
                                      ('untagged', frame, None),
                                      ('VLAN 30', frame[:12] + struct.pack('>HH', 0x8100, 30) + frame[12:], None)):
        net.queue = [(0, arriving)]
        tester.call(receive)
        n = tester.uc.reg_read(UC_ARM_REG_R0)
        n = n - (1 << 32) if n >= 1 << 31 else n
        got = bytes(tester.uc.mem_read(FRAME, max(n, 0)))
        if (expected is None and n != 0) or (expected is not None and (n != len(expected) or got != expected)):
            problems.append(f'{label} frame received as {n} bytes')
    return problems


def check_ping_tick(image, sram, symbols):
    """The 1 ms tick's vendor PING/SCAN network work (0x08015114(4)): never in Ping monitor (state 11),
    still for IP SCAN (state 12)."""
    tester = LinkTester(image, sram, symbols['text'] & ~1)
    tester.net = net = Network(tester)
    work = symbols['lpm_ping_tick_work'] & ~1
    problems = []
    for state, expected in ((11, []), (12, [4])):
        net.app_commands.clear()
        tester.uc.mem_write(SYS_STATE, bytes([state]))
        tester.call(work, 4, 0, 0)
        if net.app_commands != expected:
            problems.append(f'state {state}: app commands {net.app_commands}')
    return problems


def read_state(uc):
    fields = STATE.unpack(bytes(uc.mem_read(LINK, STATE.size)))
    rtt, internet_rtt, dns_rtt, rtt6, internet6_rtt, lifetime, steps = fields[:7]
    flags = fields[7:22]
    dns, dns_ip = fields[26], fields[27]
    return {'steps': [STEPS[s] if s < len(STEPS) else f'?{s}' for s in steps], 'internet': flags[3],
            'dns_answer': flags[4], 'dns': dns, 'dns_ip': dns_ip, 'rtt_us': rtt, 'internet_rtt_us': internet_rtt,
            'dns_rtt_us': dns_rtt, 'rtt6_us': rtt6, 'internet6_rtt_us': internet6_rtt, 'top': flags[12],
            'drawn_top': flags[13], 'rcr': flags[14], 'slaac': fields[33], 'leased': fields[34]}


def linked_symbols():
    """Addresses of the added code, from the same link the build does (needs clang and lld)."""
    import build_patched_image as b
    patch_dir, ui = ROOT / 'src' / 'firmware_patch', ROOT / 'src' / 'lpm_ui'
    return b.link_added_code(b.home_ui_sources(), patch_dir / 'home_ui.ld', includes=[ui, patch_dir],
                             keep=b.HOME_UI_KEEP)[1]


def check_scroll(tester, key, draw):
    """UP and DOWN, through lpm_home_key as the key events arrive: the list moves a row at a time and stops at
    both ends, the menu selection stays, and only the Link status redraw is posted. Each new position on the
    way down is drawn, as the GUI task would, so every row is drawn at least once. Returns the problems and
    the last top row."""
    problems = []
    selection = bytes(tester.uc.mem_read(MENU_SELECTION, 1))
    tester.uc.mem_write(LINK + TOP, b'\x00')
    tester.messages.clear()
    downs, ups = [], []
    for _ in range(12):
        tester.call(key, KEY_DOWN)
        downs.append(read_state(tester.uc)['top'])
        if len(downs) < 2 or downs[-1] != downs[-2]:
            tester.call(draw)
    last = downs[-1]
    for _ in range(12):
        tester.call(key, KEY_UP)
        ups.append(read_state(tester.uc)['top'])
    if not last or downs != [min(i + 1, last) for i in range(12)] or ups != [max(last - i - 1, 0) for i in range(12)]:
        problems.append(f'scroll positions {downs} then {ups}')
    if bytes(tester.uc.mem_read(MENU_SELECTION, 1)) != selection:
        problems.append('UP/DOWN changed the menu selection')
    if set(tester.messages) != {0x0C}:
        problems.append(f'UP/DOWN posted messages {sorted(set(tester.messages))}')
    return problems, last


def check_port_mark(fb):
    """The vendor's green arrow at the top-left port (arrow 2, x 3-19, y 4-12), the status text clear of it,
    and nothing painted where the vendor's other arrows go (0 and 1 on the right edge, 3 and 4 along the top)."""
    def pixels(x0, y0, x1, y1):
        return {fb[y * P.W + x] for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)}
    problems = []
    arrow = pixels(3, 4, 19, 12)
    if GREEN not in arrow or arrow - {GREEN, BACKGROUND}:
        problems.append(f'top-left arrow colours {sorted(map(hex, arrow))}')
    for name, box in (('0', (232, 70, 236, 102)), ('1', (232, 186, 236, 218)), ('3', (112, 4, 128, 12)),
                      ('4', (220, 4, 236, 12))):
        if pixels(*box) & {GREEN, VENDOR_PANEL}:
            problems.append(f'arrow {name} drawn')
    return problems


# ---- VLAN test and Duplicate IP (tools.h) ------------------------------------------

TOOL_VLAN, TOOL_DUPLICATE = 1, 2
DUP = 0x20020600  # duplicate_state in duplicate_ip.c
DUP_STATE = struct.Struct('<4sBBBB24s24s4s4s')
ACTION_UP, ACTION_DOWN, ACTION_LEFT, ACTION_RIGHT = 2, 3, 4, 5  # lpm_home_key's actions, as lpm_link_key gets them

MAC_SYNOLOGY, MAC_INTRUDER = bytes.fromhex('001132445566'), bytes.fromhex('00deadbeef01')
VLAN_20 = [ACTION_LEFT, ACTION_UP, ACTION_UP, ACTION_RIGHT, ACTION_DOWN]  # from VLAN 0001: tens to 2, ones to 0

TOOL_SCENARIOS = {
    # name -> (tool, network model settings, actions before M, checks)
    'vlan-trunk': (TOOL_VLAN, dict(trunk={20}), VLAN_20,
                   dict(steps=['PASS'] * 6, vlan=20, drawn=['VLAN ID', 'tagged'])),
    'vlan-not-carried': (TOOL_VLAN, dict(trunk={30}), VLAN_20,
                         dict(steps=['PASS', 'FAIL'] + ['SKIPPED'] * 4, vlan=20)),
    'duplicate-free': (TOOL_DUPLICATE, dict(), [],
                       dict(steps=['PASS', 'PASS', 'PASS', 'PASS'], claimants=0, gateways=1,
                            drawn=['answers to no other host', '192.168.1.1 at 00:1A:2B:3C:00:01'])),
    'duplicate-conflict': (TOOL_DUPLICATE, dict(conflict_mac=MAC_INTRUDER, gateway_twin=MAC_SYNOLOGY), [],
                           dict(steps=['PASS', 'PASS', 'FAIL', 'FAIL'], claimants=1, gateways=2,
                                drawn=['1 other host: 00:DE:AD:BE:EF:01', '2 MACs'])),
    'duplicate-no-link': (TOOL_DUPLICATE, dict(link=False), [],
                          dict(steps=['FAIL', 'SKIPPED', 'SKIPPED', 'SKIPPED'], probes=0)),
}


def run_tool_scenario(image, sram, symbols, entries, tool, settings, actions, checks, out, name):
    """Select the tool on the home list, open it (nothing may run), edit with the arrows, press M, and draw
    the result. Returns the problems."""
    open_, frame, draw, run = entries
    key = symbols['lpm_home_key'] & ~1
    tester = LinkTester(image, sram, symbols['text'] & ~1)
    for base, size in ((0x40000000, 0x30000), (0x50000000, 0x10000)):  # the key path touches timers and GPIO
        tester.uc.mem_map(base, size)
    tester.uc.mem_write(KEY_QUEUE, struct.pack('<I', 0x20001000))
    # The home list left on the tool (lpm_home_key sets both), VLAN 1 with the ones digit selected.
    tester.uc.mem_write(TOOLS, struct.pack('<IIHHBBBBB', TOOLS_MAGIC, 0, 0, 1, 5 + tool, 0xEE, 3, 0, 0))
    tester.uc.mem_write(MENU_SELECTION, bytes([8]))
    tester.uc.mem_write(DUP, b'\xa5' * DUP_STATE.size)  # whatever the RAM held: the open must clear it
    tester.net = net = Network(tester, **settings)
    before = {region: bytes(tester.uc.mem_read(*region)) for region in (IP_CONFIG, CUST_INFO)}
    problems = []
    tester.call(open_)
    if net.us:
        problems.append('the network was used before M')
    if tester.uc.mem_read(TOOLS + 13, 1)[0] != tool:
        problems.append(f'opened tool {tester.uc.mem_read(TOOLS + 13, 1)[0]}')
    tester.call(frame)
    tester.call(draw)
    P.png(out / f'{name}-waiting.png', tester.fb, P.W, P.H)
    for action in actions:
        tester.call(key, action)
    tester.drawn.clear()
    try:
        tester.call(run)  # M (event 0x0B)
    except SystemExit as error:
        problems.append(f'run: {error}')
    tester.call(frame)
    tester.call(draw)
    P.png(out / f'{name}.png', tester.fb, P.W, P.H)
    problems += list(net.errors)
    tools = struct.unpack('<IIHHBBBBB', bytes(tester.uc.mem_read(TOOLS, 17)))
    if tools[1] == TAGGING_MAGIC:
        problems.append('still tagging after the run')
    if tools[7]:
        problems.append('the run is still marked running')
    if tester.uc.mem_read(NET_BUSY, 1)[0]:
        problems.append('network busy flag left set')
    if net.keys_dropped:
        problems.append(f'{net.keys_dropped} key presses dropped')
    for region, old in before.items():
        if bytes(tester.uc.mem_read(*region)) != old:
            problems.append(f'{region[0]:#010x} changed: nothing may be saved')
    if tool == TOOL_VLAN:
        state = read_state(tester.uc)
        steps = [state['steps'][r] for r in IPV4_ROWS]
        if steps != checks['steps']:
            problems.append(f'steps {steps}')
        if tools[3] != checks['vlan']:
            problems.append(f'VLAN {tools[3]}')
        # Our own frames (ARP replies, pings, DNS) follow an address; each must carry the VLAN.
        if set(net.vlans_sent) - {checks['vlan']} or (steps[1] == 'PASS' and not net.vlans_sent):
            problems.append(f'frames tagged {sorted(set(net.vlans_sent))}')
        if not net.dhcp_vlans or set(net.dhcp_vlans) != {checks['vlan']}:
            problems.append(f'DHCP on VLANs {net.dhcp_vlans}')
    else:
        d = DUP_STATE.unpack(bytes(tester.uc.mem_read(DUP, DUP_STATE.size)))
        state = read_state(tester.uc)
        steps = state['steps'][:2] + [STEPS[x] for x in d[0][2:]]
        if steps != checks['steps']:
            problems.append(f'steps {steps}')
        for field, index in (('claimants', 2), ('gateways', 3)):
            if field in checks and d[index] != checks[field]:
                problems.append(f'{field} {d[index]}')
        probes = checks.get('probes', 3 if checks['steps'][1] == 'PASS' else 0)
        if net.arp_probes != [OUR_IP] * probes:
            problems.append(f'probes {[p.hex() for p in net.arp_probes]}')
    drawn = ' | '.join(tester.drawn)
    problems += [f'"{text}" not drawn' for text in checks.get('drawn', []) if text not in drawn]
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path,
                        default=ROOT / 'firmware' / 'patched' / 'home-ui' / 'LPM-10C_TX_V2.0.7_260907.bin')
    parser.add_argument('--sram', type=Path, default=ROOT / 'tests' / 'fixtures' / 'sram-sanitized.bin')
    parser.add_argument('--out', type=Path, default=ROOT / 'output' / 'link-status')
    parser.add_argument('--verbose', action='store_true', help='print every string each screen draws')
    parser.add_argument('--only', nargs='+', metavar='SCENARIO', help='run only these scenarios')
    args = parser.parse_args()
    try:
        import unicorn  # noqa: F401
    except ImportError:
        raise SystemExit('Needs unicorn: .venv/bin/pip install unicorn (see README)')

    image, sram = args.image.read_bytes(), args.sram.read_bytes()
    open_, frame, draw, run = (P.find_entry(image, site) for site in (OPEN_SITE, FRAME_SITE, DRAW_SITE, RUN_SITE))
    vendor_targets = (0x0800BAE4, 0x0800EE8C, 0x0800BB0C, 0x0800BD98)  # QC open, tool frame, QC draw, QC count
    if any(entry == vendor for entry, vendor in zip((open_, frame, draw, run), vendor_targets)):
        raise SystemExit(f'{args.image} does not redirect the QC test to Link status')
    symbols = linked_symbols()
    if symbols['lpm_link_open'] & ~1 != open_:
        raise SystemExit(f'{args.image} was not built from the current sources: rebuild it first')
    key = symbols['lpm_home_key'] & ~1
    args.out.mkdir(parents=True, exist_ok=True)
    failures = []
    deepest = 0
    for name, (settings, expected, fragments) in SCENARIOS.items():
        if args.only and name not in args.only:
            continue
        tester = LinkTester(image, sram, symbols['text'] & ~1)
        tester.net = net = Network(tester, **settings)
        before = {region: bytes(tester.uc.mem_read(*region)) for region in (IP_CONFIG, CUST_INFO)}
        tester.uc.mem_write(LINK, b'\xa5' * STATE.size)  # whatever the RAM held: the open must clear it
        top = tester.lowest_sp
        started_us = net.us
        tester.call(open_)
        opened = read_state(tester.uc)
        if net.us != started_us or any(step != '--' for step in opened['steps']):  # no network routine ran
            problems_open = [f'checks ran before M: {opened["steps"]}']
        else:
            problems_open = []
        if name == 'all-pass':  # the screen as it opens
            tester.call(frame)
            tester.call(draw)
            P.png(args.out / 'link-status-waiting.png', tester.fb, P.W, P.H)
            if 'Press M to start the checks' not in ' | '.join(tester.drawn):
                problems_open.append('no "Press M" hint on opening')
        tester.call(run)  # M (event 0x0B)
        run_stack = top - tester.lowest_sp
        state = read_state(tester.uc)
        # What the GUI task does with the posted messages: the frame for 0x36, the rows for 0x0C. The run
        # leaves the list scrolled to its last check; the top of the list is drawn first.
        last = state['top']
        tester.lowest_sp = top
        tester.uc.mem_write(LINK + TOP, b'\x00')
        tester.call(frame)
        tester.call(draw)
        P.png(args.out / f'{name}.png', tester.fb, P.W, P.H)
        arrow_problems = check_port_mark(tester.fb)
        tester.uc.mem_write(LINK + TOP, bytes([last]))
        tester.call(draw)  # a scroll: the view is cleared and redrawn from the new top
        draw_stack = top - tester.lowest_sp
        deepest = max(deepest, run_stack, draw_stack)
        P.png(args.out / f'{name}-end.png', tester.fb, P.W, P.H)
        problems = list(net.errors) + arrow_problems + problems_open
        steps = [state['steps'][r] for r in IPV4_ROWS] + [state['steps'][r] for r in IPV6_ROWS]
        if steps != expected:
            problems.append(f'steps {steps}, expected {expected}')
        if tester.uc.mem_read(SYS_STATE, 1)[0] != 8:
            problems.append('not in system state 8')
        if tester.uc.mem_read(NET_BUSY, 1)[0] != 0:
            problems.append('network busy flag left set')
        for region, old in before.items():
            if bytes(tester.uc.mem_read(*region)) != old:
                problems.append(f'{region[0]:#010x} changed: the result must not be saved')
        if set(tester.messages) - {0x0C, 0x36} or tester.messages[:1] != [0x36]:
            problems.append(f'messages {tester.messages}')
        if settings.get('arp_first') and not net.arp_answered:
            problems.append("the gateway's ARP request was not answered")
        if name == 'all-pass' and not 350 <= state['rtt_us'] <= 600:
            problems.append(f'RTT {state["rtt_us"]} us, expected about 400')
        if name == 'all-pass' and not INTERNET_RTT_US <= state['internet_rtt_us'] <= INTERNET_RTT_US + 100:
            problems.append(f'Internet RTT {state["internet_rtt_us"]} us, expected about {INTERNET_RTT_US}')
        # The Internet check stops at the first address that answers; 8.8.8.8 is only a fallback.
        internet_targets = [t for t in net.echo_targets if t != net.gateway]
        if expected[4] == 'PASS' and CLOUDFLARE in net.internet and GOOGLE in internet_targets:
            problems.append('pinged 8.8.8.8 although 1.1.1.1 answered')
        if expected[4] == 'FAIL' and internet_targets != [CLOUDFLARE] * 2 + [GOOGLE] * 2:
            problems.append(f'Internet echoes {[t.hex() for t in internet_targets]}, expected two to each')
        # DNS: the DHCP server first, then 1.1.1.1 only if it failed (and is not the same), two tries each.
        if expected[5] != 'SKIPPED':
            dhcp_dns = net.dhcp_dns or bytes(4)
            if state['dns'] != dhcp_dns:
                problems.append(f'DNS server read from DHCP {state["dns"].hex()}, expected {dhcp_dns.hex()}')
            asked = []
            if net.dhcp_dns:
                asked += [net.dhcp_dns] * (1 if expected[5] == 'PASS' else 2)
            if expected[5] != 'PASS' and net.dhcp_dns != CLOUDFLARE:
                asked += [CLOUDFLARE] * (1 if expected[5] == 'WARN' else 2)
            if net.dns_queries != asked:
                problems.append(f'DNS queries to {[q.hex() for q in net.dns_queries]}, expected {[q.hex() for q in asked]}')
            if expected[5] in ('PASS', 'WARN') and state['dns_ip'] != EXAMPLE_COM:
                problems.append(f'resolved {state["dns_ip"].hex()}, expected {EXAMPLE_COM.hex()}')
        elif net.dns_queries:
            problems.append('DNS queried although the check was skipped')
        if name == 'all-pass' and not 3000 <= state['dns_rtt_us'] <= 3100:
            problems.append(f'DNS RTT {state["dns_rtt_us"]} us, expected about 3000')
        # IPv6: all multicast passes only while the IPv6 checks run, and the vendor value is restored.
        v6 = expected[6:]
        if not settings.get('link', True):
            if net.rcr_writes or net.solicitations or net.probes or net.dhcp6_messages:
                problems.append('IPv6 traffic without a link')
        else:
            if net.rcr_writes != [VENDOR_RCR | RCR_ALL, VENDOR_RCR] or net.rcr != VENDOR_RCR or state['rcr']:
                problems.append(f'receive control writes {[hex(v) for v in net.rcr_writes]}')
            # Duplicate address detection: the link-local address first, then each new address.
            probes = [net.link_local()] + ([net.slaac()] if v6[1] in ('PASS', 'FAIL') else []) \
                + ([LEASED] if net.dhcp6 in ('lease', 'rapid', 'advertise') else [])
            if net.probes != probes:
                problems.append(f'address probes {[p.hex() for p in net.probes]}, expected {[p.hex() for p in probes]}')
            solicitations = 1 if net.ra and net.lifetime else 4
            if net.solicitations != solicitations:
                problems.append(f'{net.solicitations} router solicitations, expected {solicitations}')
            dhcp6 = {None: [1, 1], 'lease': [1, 3], 'rapid': [1], 'advertise': [1, 3, 3], 'refuse': [1]}[net.dhcp6]
            if net.dhcp6_messages != dhcp6:
                problems.append(f'DHCPv6 messages {net.dhcp6_messages}, expected {dhcp6}')
            if v6[1] == 'PASS' and state['slaac'] != net.slaac():
                problems.append(f'SLAAC address {state["slaac"].hex()}')
            if v6[4] in ('PASS', 'FAIL'):
                internet6 = [t for t in net.echo6_targets if t != ROUTER_LL]
                wanted = [CLOUDFLARE6] if v6[4] == 'PASS' and CLOUDFLARE6 in net.internet6 else \
                    [CLOUDFLARE6] * 2 + [GOOGLE6] * (1 if v6[4] == 'PASS' else 2)
                if internet6 != wanted:
                    problems.append(f'IPv6 Internet echoes {[t.hex() for t in internet6]}')
                global_source = net.slaac() if v6[1] == 'PASS' else LEASED
                if v6[4] == 'PASS' and global_source not in net.resolved:
                    problems.append("the router's neighbour solicitation was not answered")
            if name == 'all-pass' and not 600 <= state['rtt6_us'] <= 700:
                problems.append(f'IPv6 RTT {state["rtt6_us"]} us, expected about 600')
        scroll_problems, bottom = check_scroll(tester, key, draw)
        problems += scroll_problems
        drawn = ' | '.join(tester.drawn)
        problems += [f'"{text}" not drawn' for text in fragments if text not in drawn]
        if settings.get('link', True) and last != bottom:
            problems.append(f'the run ended scrolled to row {last}, not to the end ({bottom})')
        status = 'ok' if not problems else 'FAIL: ' + '; '.join(problems)
        print(f'{name:22} {" ".join(f"{s[:4]:4}" for s in steps)}  {net.us / 1e6:4.1f} s  {status}')
        if args.verbose:
            print('    ' + drawn)
        if problems:
            failures.append(name)
    ping_entries = [P.find_entry(image, site) for site in (PING_OPEN_SITE, PING_DRAW_SITE)]
    if ping_entries[0] != symbols['lpm_ping_open'] & ~1:
        raise SystemExit(f'{args.image} does not open Ping monitor from the PING slot')
    for name, (settings, checks) in PING_SCENARIOS.items():
        if args.only and name not in args.only:
            continue
        problems, seconds = run_ping_scenario(image, sram, symbols, (ping_entries[0], frame, ping_entries[1]),
                                              settings, checks, args.out, name)
        status = 'ok' if not problems else 'FAIL: ' + '; '.join(problems)
        print(f'{name:28} {seconds:6.1f} s  {status}')
        if problems:
            failures.append(name)
    if not args.only or 'vlan-wrappers' in args.only:
        problems = check_vlan_wrappers(image, sram, symbols)
        print(f'{"vlan-wrappers":28} {"ok" if not problems else "FAIL: " + "; ".join(problems)}')
        if problems:
            failures.append('vlan-wrappers')
    if not args.only or 'ping-tick' in args.only:
        problems = check_ping_tick(image, sram, symbols)
        print(f'{"ping-tick":28} {"ok" if not problems else "FAIL: " + "; ".join(problems)}')
        if problems:
            failures.append('ping-tick')
    for name, (tool, settings, actions, checks) in TOOL_SCENARIOS.items():
        if args.only and name not in args.only:
            continue
        problems = run_tool_scenario(image, sram, symbols, (open_, frame, draw, run), tool, settings, actions,
                                     checks, args.out, name)
        print(f'{name:28} {"ok" if not problems else "FAIL: " + "; ".join(problems)}')
        if problems:
            failures.append(name)
    for name, (settings, expected, fragments) in SWITCH_SCENARIOS.items():
        if args.only and name not in args.only:
            continue
        problems, seconds = run_switch_scenario(image, sram, symbols, (open_, frame, draw, run), settings, expected,
                                                fragments, args.out, name)
        status = 'ok' if not problems else 'FAIL: ' + '; '.join(problems)
        print(f'{name:22} {" ".join(f"{s[:4]:4}" for s in expected)}  {seconds:4.1f} s  {status}')
        if problems:
            failures.append(name)
    print(f'Deepest stack use: {deepest} bytes')
    total = len(args.only) if args.only else 2 * (len(SCENARIOS) + len(SWITCH_SCENARIOS) + len(TOOL_SCENARIOS)) + len(PING_SCENARIOS)
    print(f'Wrote {total} screens to {args.out}/')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
