#!/usr/bin/env python3
"""Measure the tester's UART baud rate by sampling the line with an FT232H.

The FT232H runs in synchronous bit-bang mode with every pin set as an input,
so it drives nothing: it only samples the line wired to D1 (board TX -> D1,
GND -> GND, the same wiring as uart_log.py). Pulse widths give the bit time,
which is snapped to the nearest standard rate. Power-cycle the tester while it
listens; a boot burst is enough.

  .venv/bin/python scripts/uart_baud.py --seconds 20

Needs pyftdi in .venv and Homebrew libusb (see README).
"""
import argparse
import os
import sys
import time

STANDARD_BAUDS = (1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 74880, 115200, 128000, 230400, 250000,
                  256000, 460800, 500000, 921600, 1000000, 1500000, 2000000)


def runs_in_chunk(samples, mask):
    """(level, length) of each complete run of the masked bit; partial runs at chunk edges are dropped."""
    runs = []
    if not samples:
        return runs
    level, length = bool(samples[0] & mask), 0
    first = True
    for byte in samples:
        bit = bool(byte & mask)
        if bit == level:
            length += 1
            continue
        if not first:
            runs.append((level, length))
        first = False
        level, length = bit, 1
    return runs


def estimate_bit_time(runs, sample_rate):
    """Bit time in samples, from runs that should be whole multiples of one bit.

    The shortest frequently seen run is taken as a first guess, then refined
    by fitting every run up to 9 bits long (a start bit plus 8 data bits) as an
    integer number of bits. Long idle-high runs are ignored.
    """
    lengths = sorted(length for _, length in runs if length >= 2)
    if len(lengths) < 20:
        return None
    # Ignore the shortest 2% as glitches or edge jitter.
    guess = lengths[len(lengths) // 50]
    total_samples = total_bits = 0
    for length in lengths:
        bits = round(length / guess)
        if 1 <= bits <= 9:
            total_samples += length
            total_bits += bits
    return total_samples / total_bits if total_bits else None


def nearest_standard(baud):
    best = min(STANDARD_BAUDS, key=lambda b: abs(b - baud) / b)
    return best, abs(best - baud) / best


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--url', default='ftdi://ftdi:232h/1')
    parser.add_argument('--rate', type=int, default=1000000,
                        help='sample rate in Hz (default 1 MHz; use 4000000 or more above 250000 baud, 200000 at 9600 and below)')
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--pin', type=int, default=1, help='ADBUS pin wired to board TX (default D1)')
    args = parser.parse_args()

    os.environ.setdefault('DYLD_FALLBACK_LIBRARY_PATH', '/opt/homebrew/lib:/usr/local/lib')
    try:
        from pyftdi.ftdi import Ftdi
    except ImportError:
        raise SystemExit('Needs pyftdi: run with .venv/bin/python (see README)')

    ftdi = Ftdi()
    # direction=0: all eight pins are inputs, so the bytes written below only clock the sampler.
    # Timing 384-sample rounds at 20 and 50 kHz matched the rate pyftdi returns within USB overhead.
    rate = ftdi.open_bitbang_from_url(args.url, direction=0x00, sync=True, baudrate=args.rate, latency=1)
    mask = 1 << args.pin
    # In synchronous mode each written byte returns one sample. Rounds must stay below the 512-byte
    # USB packet (2 status bytes each) or the receive buffer fills and the write times out.
    chunk = 448
    runs, low_seen, levels = [], 0, [0, 0]
    print(f'Sampling D{args.pin} at {rate / 1e6:.3f} MHz for {args.seconds:g} s. Power-cycle the tester now.')
    end = time.monotonic() + args.seconds
    try:
        while time.monotonic() < end:
            ftdi.write_data(bytes(chunk))
            samples = ftdi.read_data_bytes(chunk, attempt=10)
            for byte in samples:
                levels[bool(byte & mask)] += 1
            chunk_runs = runs_in_chunk(samples, mask)
            low_seen += sum(1 for level, _ in chunk_runs if not level)
            runs.extend(chunk_runs)
    except KeyboardInterrupt:
        pass
    finally:
        ftdi.close()

    sampled = sum(levels)
    # Sampling only runs while a round is in flight, so short bursts can fall into the USB gaps.
    coverage = sampled / (rate * args.seconds)
    print(f'{sampled} samples: {levels[1] / max(sampled, 1):.1%} high, {low_seen} low pulses, '
          f'{coverage:.0%} of the time sampled')
    if levels[1] < levels[0]:
        print('Line idles LOW: the signal may be inverted, or D1 is not on a UART TX pin.')
    bit = estimate_bit_time(runs, rate)
    if bit is None:
        print('Too few transitions to measure. Check TX -> D1 and GND, and power-cycle during sampling.')
        return 1
    baud = rate / bit
    best, error = nearest_standard(baud)
    print(f'Measured {baud:,.0f} baud ({bit:.2f} samples per bit); nearest standard {best} ({error:.1%} off).')
    if bit < 4:
        # At about 2 samples per bit, runs of one bit are often missed, so the shortest runs seen are
        # two bits long and the figure comes out near half the real rate (1 Mbaud read as 500000).
        print(f'Only {bit:.1f} samples per bit: the real rate may be a multiple of this.\n'
              f'Rerun with --rate {int(rate * 4)} or more before logging.')
        return 1
    print(f'Log with: .venv/bin/python scripts/uart_log.py --port {args.url} --baud {best}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
