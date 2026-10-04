#!/usr/bin/env python3
"""Read-only validator for the verified Rise 32^3 TEX28 profile, not a converter."""

from __future__ import annotations

import argparse
import math
import pathlib
import sys
from dataclasses import dataclass

from validate_cube import CubeError, Metrics, identity_samples, load_cube, measure


HEADER = bytes.fromhex(
    "54 45 58 00 1c 00 00 00 20 00 20 00 20 00 01 10 "
    "1d 00 00 00 ff ff ff ff 00 00 00 00 00 08 00 00 "
    "00 00 00 00 00 00 00 00 38 00 00 00 00 00 00 00 "
    "80 00 00 00 00 10 00 00"
)
SAMPLE_COUNT = 32**3
FILE_SIZE = 56 + SAMPLE_COUNT * 4


class RiseTexError(ValueError):
    pass


@dataclass(frozen=True)
class RiseTex:
    payload: bytes

    @property
    def samples(self) -> tuple[tuple[float, float, float], ...]:
        return tuple(tuple(self.payload[i + c] / 255.0 for c in range(3))
                     for i in range(0, len(self.payload), 4))


def load_rise_tex(path: pathlib.Path) -> RiseTex:
    try:
        # Read at most one byte beyond this fixed profile. Malformed huge files
        # do not need to be loaded wholesale just to reject their length.
        with path.open("rb") as stream:
            data = stream.read(FILE_SIZE + 1)
    except OSError as exc:
        raise RiseTexError(f"cannot read {path}: {exc}") from exc
    if len(data) != FILE_SIZE:
        raise RiseTexError(f"expected exactly {FILE_SIZE} bytes for Rise TEX28, found {len(data)} (at most one extra byte read)")
    if data[:56] != HEADER:
        raise RiseTexError("header does not match the verified 32^3 RGBA8 sRGB Rise TEX28 profile")
    payload = data[56:]
    if any(payload[i] != 255 for i in range(3, len(payload), 4)):
        raise RiseTexError("Rise LUT alpha must be 255 at every node")
    return RiseTex(payload)


def compare_cube(tex: RiseTex, cube_path: pathlib.Path, clamp: bool = False) -> Metrics:
    cube = load_cube(cube_path)
    if cube.size != 32:
        raise RiseTexError("comparison CUBE must be 32^3; this tool does not resample or convert")
    if not clamp and any(value < 0 or value > 1 for row in cube.samples for value in row):
        raise RiseTexError("comparison CUBE contains RGB outside 0-1; use --clamp only if export used explicit clipping")
    expected = (tuple(min(1.0, max(0.0, value)) for value in row) for row in cube.samples) if clamp else cube.samples
    return measure(tex.samples, expected)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser("inspect", help="check exact header, dimensions, length and alpha")
    inspect.add_argument("tex", type=pathlib.Path)
    identity = commands.add_parser("identity", help="compare against ideal red-fastest RGB identity")
    identity.add_argument("tex", type=pathlib.Path)
    identity.add_argument("--tolerance", type=float, default=0.5 / 255.0 + 3e-8,
                          help="default: half a byte step plus float32 identity error; FP16 may need a larger bound")
    compare = commands.add_parser("compare-cube", help="compare node values against a float 32^3 export (read-only)")
    compare.add_argument("tex", type=pathlib.Path)
    compare.add_argument("cube", type=pathlib.Path)
    compare.add_argument("--clamp", action="store_true", help="compare against clipped CUBE values explicitly")
    compare.add_argument("--tolerance", type=float, default=0.5 / 255.0 + 1e-8)
    args = parser.parse_args()
    try:
        tex = load_rise_tex(args.tex)
        print("Verified Rise TEX28: 32 x 32 x 32, DXGI 29, RGBA, red-fastest, alpha 255, 131128 bytes")
        if args.command == "inspect":
            return 0
        if not math.isfinite(args.tolerance) or args.tolerance < 0:
            raise RiseTexError("tolerance must be finite and non-negative")
        metrics = measure(tex.samples, identity_samples(32)) if args.command == "identity" else compare_cube(tex, args.cube, args.clamp)
        print(f"Maximum absolute RGB error: {metrics.maximum_absolute:.12g}")
        print(f"Mean absolute RGB error: {metrics.mean_absolute:.12g}")
        print(f"RMS RGB error: {metrics.rms:.12g}")
        print("Node metrics only: not a validation of GPU execution, interpolation, native loading or SDR/HDR equivalence.")
        return 0 if metrics.maximum_absolute <= args.tolerance else 1
    except (RiseTexError, CubeError) as exc:
        print(f"Validation failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
