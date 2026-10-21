# Documentation

Investigation notes, in roughly the order the work was done:

| Document | What it covers |
| --- | --- |
| [lpm-10c-investigation.md](lpm-10c-investigation.md) | The vendor update package, the USB boot drive and a first static pass over the TX firmware |
| [lpm-10c-hardware-capabilities.md](lpm-10c-hardware-capabilities.md) | What the hardware can do, from firmware strings and the board |
| [lpm-10c-board-inspection.md](lpm-10c-board-inspection.md) | Chips on the transmitter board, the debug header, UART console and SWD identification |
| [lpm-10c-network-analysis.md](lpm-10c-network-analysis.md) | The vendor IPv4 stack: DM9051 driver, ARP, ICMP, DHCP |
| [lpm-10c-ui-and-feature-proposal.md](lpm-10c-ui-and-feature-proposal.md) | The redesigned interface and feature proposal |
| [lpm-ui-implementation.md](lpm-ui-implementation.md) | The portable UI module in `src/lpm_ui` and its host simulator |
| [lpm-10c-bootloader-analysis.md](lpm-10c-bootloader-analysis.md) | The update path, every firmware patch, the flash wear fix, freed space, the home screen and Link status |

Paths under `evidence/` (UART captures, SWD dumps, board photos, disassembly
listings) and `firmware/` (vendor files and builds) refer to the author's working
copy. They are not published: the captures come from one person's unit, and the
firmware belongs to FNIRSI. The scripts in `scripts/` regenerate the listings
from your own copy of the vendor update.
