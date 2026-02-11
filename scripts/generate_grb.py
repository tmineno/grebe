#!/usr/bin/env python3
"""Generate .grb binary files for grebe file playback testing.

Usage:
    python scripts/generate_grb.py --channels=1 --rate=100e6 --duration=5 --waveform=sine -o ./tmp/test.grb
"""

import argparse
import math
import struct
import sys
import numpy as np

GRB_MAGIC = 0x31425247  # 'GRB1' little-endian
GRB_VERSION = 1
GRB_HEADER_SIZE = 32  # bytes


def generate_waveform(waveform: str, num_samples: int, frequency: float,
                      sample_rate: float, phase_offset: float = 0.0) -> np.ndarray:
    """Generate int16 waveform samples."""
    t = np.arange(num_samples, dtype=np.float64) / sample_rate
    phase = 2.0 * math.pi * frequency * t + phase_offset

    if waveform == "sine":
        samples = np.sin(phase) * 32767.0
    elif waveform == "square":
        samples = np.where(np.sin(phase) >= 0, 32767.0, -32768.0)
    elif waveform == "sawtooth":
        norm = np.mod(frequency * t + phase_offset / (2.0 * math.pi), 1.0)
        samples = (2.0 * norm - 1.0) * 32767.0
    elif waveform == "noise":
        rng = np.random.default_rng(42)
        samples = rng.integers(-32768, 32768, size=num_samples, dtype=np.int64).astype(np.float64)
    else:
        raise ValueError(f"Unknown waveform: {waveform}")

    return np.clip(samples, -32768, 32767).astype(np.int16)


def main():
    parser = argparse.ArgumentParser(description="Generate .grb binary files")
    parser.add_argument("--channels", type=int, default=1, help="Number of channels (1-8)")
    parser.add_argument("--rate", type=float, default=1e6, help="Sample rate in Hz")
    parser.add_argument("--duration", type=float, default=1.0, help="Duration in seconds")
    parser.add_argument("--waveform", default="sine",
                        choices=["sine", "square", "sawtooth", "noise"],
                        help="Waveform type")
    parser.add_argument("--frequency", type=float, default=1000.0,
                        help="Waveform frequency in Hz")
    parser.add_argument("-o", "--output", required=True, help="Output file path")
    args = parser.parse_args()

    if not 1 <= args.channels <= 8:
        print(f"Error: channels must be 1-8, got {args.channels}", file=sys.stderr)
        return 1

    num_samples = int(args.rate * args.duration)
    total_bytes = GRB_HEADER_SIZE + args.channels * num_samples * 2

    print(f"Generating: {args.channels}ch x {num_samples} samples "
          f"({args.duration}s @ {args.rate:.0f} SPS)")
    print(f"Waveform: {args.waveform} @ {args.frequency:.0f} Hz")
    print(f"File size: {total_bytes / (1024*1024):.1f} MB")

    # Write header
    header = struct.pack("<IIIIdQ",
                         GRB_MAGIC,
                         GRB_VERSION,
                         args.channels,
                         0,  # reserved
                         args.rate,
                         num_samples)
    assert len(header) == GRB_HEADER_SIZE

    with open(args.output, "wb") as f:
        f.write(header)

        # Write channel-major: all samples for ch0, then ch1, etc.
        for ch in range(args.channels):
            phase_offset = math.pi * ch / args.channels
            samples = generate_waveform(args.waveform, num_samples,
                                        args.frequency, args.rate, phase_offset)
            f.write(samples.tobytes())
            print(f"  Ch {ch}: {len(samples)} samples written")

    print(f"Written: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
