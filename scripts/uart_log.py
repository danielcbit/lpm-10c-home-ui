#!/usr/bin/env python3
"""Listen to the tester's UART and save what it prints, receive-only.

Nothing is ever sent to the tester: a /dev port is opened read-only, and an
ftdi:// port is only read. Wire only board TX -> adapter RXD and GND -> GND;
leave board RX and 3V3 unconnected. Raw bytes and a timestamped text copy are
saved under evidence/uart/.

  uart_log.py --scan            try common baud rates, then pick the readable one
  uart_log.py --baud 115200     log until Ctrl-C (or --seconds)

--port takes a serial device (/dev/cu.usbserial-*, standard library only) or a
pyftdi URL such as ftdi://ftdi:232h/1, which talks to an FTDI chip through
libusb when macOS has not created a serial device for it. That needs pyftdi
(.venv, see README) and Homebrew libusb.
"""
import argparse
from datetime import datetime
import fcntl
import glob
import os
from pathlib import Path
import select
import sys
import termios
import time

ROOT = Path(__file__).resolve().parent.parent
SCAN_BAUDS = (1000000, 115200, 9600, 57600, 38400, 19200, 230400, 460800, 921600)  # the LPM-10C TX logs at 1000000
IOSSIOSPEED = 0x80085402  # macOS ioctl for rates without a termios Bxxx constant


def printable_score(data):
    """Fraction of bytes that look like log text; wrong baud rates give mostly noise."""
    if not data:
        return 0.0
    good = sum(1 for b in data if 0x20 <= b < 0x7F or b in (0x09, 0x0A, 0x0D))
    return good / len(data)


class LineStamper:
    """Turns a byte stream into complete, timestamped text lines."""

    def __init__(self):
        self.pending = b''

    def feed(self, data, stamp):
        self.pending += data
        # Drop CR rather than treat it as a line end, so a CRLF split across reads stays one line break.
        *lines, self.pending = self.pending.replace(b'\r', b'').split(b'\n')
        return [f'{stamp} {line.decode("ascii", errors="backslashreplace")}' for line in lines]

    def flush(self, stamp):
        rest, self.pending = self.pending, b''
        return [f'{stamp} {rest.decode("ascii", errors="backslashreplace")}'] if rest else []


def find_port():
    ports = sorted(glob.glob('/dev/cu.usbserial*') + glob.glob('/dev/ttyUSB*'))
    if len(ports) != 1:
        raise SystemExit(f'Pass --port (a /dev device or ftdi://ftdi:232h/1); found {ports or "no USB serial ports"}')
    return ports[0]


class TtyPort:
    """A /dev serial device opened read-only and configured raw 8N1."""

    def __init__(self, path, baud):
        self.fd = open_tty(path, baud)

    def read(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 4096) if ready else b''

    def close(self):
        os.close(self.fd)


class FtdiPort:
    """An FTDI UART reached through pyftdi/libusb; only ever read from."""

    def __init__(self, url, baud):
        # Homebrew's libusb is not on the default macOS library path.
        os.environ.setdefault('DYLD_FALLBACK_LIBRARY_PATH', '/opt/homebrew/lib:/usr/local/lib')
        try:
            from pyftdi.serialext import serial_for_url
        except ImportError:
            raise SystemExit('ftdi:// ports need pyftdi: run with .venv/bin/python (see README)')
        self.serial = serial_for_url(url, baudrate=baud, timeout=0)

    def read(self, timeout):
        end = time.monotonic() + timeout
        while True:
            data = self.serial.read(4096)
            if data or time.monotonic() >= end:
                return data
            time.sleep(0.01)

    def close(self):
        self.serial.close()


def open_port(port, baud):
    return FtdiPort(port, baud) if port.startswith('ftdi://') else TtyPort(port, baud)


def open_tty(path, baud):
    fd = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, _, _, cc = attrs
    iflag = 0
    oflag = 0
    lflag = 0
    cflag = (cflag & ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.HUPCL | termios.CRTSCTS)) \
        | termios.CS8 | termios.CREAD | termios.CLOCAL
    cc[termios.VMIN], cc[termios.VTIME] = 0, 0
    constant = getattr(termios, f'B{baud}', None)
    speed = constant if constant is not None else termios.B9600
    termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, speed, speed, cc])
    if constant is None:
        fcntl.ioctl(fd, IOSSIOSPEED, baud.to_bytes(8, sys.byteorder))
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd


def read_for(port, seconds, on_data):
    end = time.monotonic() + seconds if seconds else None
    while end is None or time.monotonic() < end:
        timeout = 0.2 if end is None else max(0.0, min(0.2, end - time.monotonic()))
        data = port.read(timeout)
        if data:
            on_data(data)


def now():
    return datetime.now().astimezone().isoformat(timespec='milliseconds')


def scan(port, seconds):
    print(f'Listening {seconds} s per rate on {port}. Power-cycle the tester or press buttons meanwhile.')
    results = []
    for baud in SCAN_BAUDS:
        reader = open_port(port, baud)
        chunks = []
        try:
            read_for(reader, seconds, chunks.append)
        finally:
            reader.close()
        data = b''.join(chunks)
        score = printable_score(data)
        results.append((score, len(data), baud))
        print(f'  {baud:>7} baud: {len(data):>6} bytes, {score:4.0%} printable  {data[:48]!r}')
    best = max(results)
    if best[1] == 0:
        print('No data at any rate: check TX -> RXD and GND, and that the tester is on.')
        return 1
    print(f'Most readable: {best[2]} baud. Log with: uart_log.py --port {port} --baud {best[2]}')
    return 0


def log(port, baud, seconds, out):
    out.mkdir(parents=True, exist_ok=True)
    base = out / f'uart-{datetime.now().strftime("%Y%m%d-%H%M%S")}-{baud}'
    stamper = LineStamper()
    reader = open_port(port, baud)
    print(f'Logging {port} at {baud} baud to {base}.{{bin,txt}}; Ctrl-C to stop.')
    with open(f'{base}.bin', 'wb') as raw, open(f'{base}.txt', 'w') as text:
        def on_data(data):
            raw.write(data)
            raw.flush()
            for line in stamper.feed(data, now()):
                print(line)
                text.write(line + '\n')
            text.flush()
        try:
            read_for(reader, seconds, on_data)
        except KeyboardInterrupt:
            pass
        finally:
            reader.close()
            for line in stamper.flush(now()):
                print(line)
                text.write(line + '\n')
    print(f'Saved {base}.bin and {base}.txt')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', help='/dev/cu.usbserial-XXXX (auto if only one) or ftdi://ftdi:232h/1')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--scan', action='store_true', help='try common baud rates and report the readable one')
    parser.add_argument('--seconds', type=float, help='stop after this long (scan default: 5 per rate)')
    parser.add_argument('--out', type=Path, default=ROOT / 'evidence' / 'uart')
    args = parser.parse_args()
    port = args.port or find_port()
    if args.scan:
        return scan(port, args.seconds or 5)
    return log(port, args.baud, args.seconds, args.out)


if __name__ == '__main__':
    sys.exit(main())
