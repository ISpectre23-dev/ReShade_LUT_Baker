#!/usr/bin/env python3
"""Strict, dependency-free validation for ReShade LUT Baker CUBE files."""

from __future__ import annotations

import argparse
import math
import pathlib
import shlex
import sys
from dataclasses import dataclass
from typing import Iterable, Sequence


@dataclass(frozen=True)
class Cube:
    size: int
    domain_min: tuple[float, float, float]
    domain_max: tuple[float, float, float]
    samples: tuple[tuple[float, float, float], ...]


@dataclass(frozen=True)
class Metrics:
    maximum_absolute: float
    mean_absolute: float
    rms: float


class CubeError(ValueError):
    pass


def _parse_triplet(tokens: Sequence[str], line_number: int, label: str) -> tuple[float, float, float]:
    if len(tokens) != 4:
        raise CubeError(f"line {line_number}: {label} requires exactly three values")
    try:
        values = tuple(float(value) for value in tokens[1:])
    except ValueError as exc:
        raise CubeError(f"line {line_number}: invalid {label} value") from exc
    if not all(math.isfinite(value) for value in values):
        raise CubeError(f"line {line_number}: {label} contains NaN or infinity")
    return values  # type: ignore[return-value]


def load_cube(path: pathlib.Path) -> Cube:
    size: int | None = None
    domain_min = (0.0, 0.0, 0.0)
    domain_max = (1.0, 1.0, 1.0)
    saw_domain_min = False
    saw_domain_max = False
    samples: list[tuple[float, float, float]] = []
    data_started = False

    try:
        lines = path.read_text(encoding="utf-8-sig").splitlines()
    except OSError as exc:
        raise CubeError(f"cannot read {path}: {exc}") from exc

    for line_number, raw_line in enumerate(lines, start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        try:
            tokens = shlex.split(line, comments=False, posix=True)
        except ValueError as exc:
            raise CubeError(f"line {line_number}: malformed quoting") from exc
        if not tokens:
            continue

        keyword = tokens[0].upper()
        if keyword == "TITLE":
            if data_started or len(tokens) != 2:
                raise CubeError(f"line {line_number}: invalid TITLE declaration")
            continue
        if keyword == "LUT_1D_SIZE":
            raise CubeError("combined or 1D LUTs are not supported by this validator")
        if keyword == "LUT_3D_SIZE":
            if data_started or size is not None or len(tokens) != 2:
                raise CubeError(f"line {line_number}: invalid LUT_3D_SIZE declaration")
            try:
                size = int(tokens[1], 10)
            except ValueError as exc:
                raise CubeError(f"line {line_number}: LUT_3D_SIZE is not an integer") from exc
            if size < 2 or size > 256:
                raise CubeError(f"line {line_number}: unreasonable LUT size {size}")
            continue
        if keyword == "DOMAIN_MIN":
            if data_started or saw_domain_min:
                raise CubeError(f"line {line_number}: duplicate or misplaced DOMAIN_MIN")
            domain_min = _parse_triplet(tokens, line_number, "DOMAIN_MIN")
            saw_domain_min = True
            continue
        if keyword == "DOMAIN_MAX":
            if data_started or saw_domain_max:
                raise CubeError(f"line {line_number}: duplicate or misplaced DOMAIN_MAX")
            domain_max = _parse_triplet(tokens, line_number, "DOMAIN_MAX")
            saw_domain_max = True
            continue

        if size is None:
            raise CubeError(f"line {line_number}: table data appears before LUT_3D_SIZE")
        if len(tokens) != 3:
            raise CubeError(f"line {line_number}: each table row must contain exactly three values")
        try:
            sample = tuple(float(value) for value in tokens)
        except ValueError as exc:
            raise CubeError(f"line {line_number}: invalid table value") from exc
        if not all(math.isfinite(value) for value in sample):
            raise CubeError(f"line {line_number}: table contains NaN or infinity")
        samples.append(sample)  # type: ignore[arg-type]
        data_started = True

    if size is None:
        raise CubeError("missing LUT_3D_SIZE")
    expected_count = size**3
    if len(samples) != expected_count:
        raise CubeError(f"expected exactly {expected_count:,} rows for {size}^3, found {len(samples):,}")
    if domain_min != (0.0, 0.0, 0.0) or domain_max != (1.0, 1.0, 1.0):
        raise CubeError("ReShade LUT Baker validation requires DOMAIN_MIN=0 and DOMAIN_MAX=1")

    return Cube(size, domain_min, domain_max, tuple(samples))


def identity_samples(size: int) -> Iterable[tuple[float, float, float]]:
    denominator = float(size - 1)
    for blue in range(size):
        for green in range(size):
            for red in range(size):
                yield red / denominator, green / denominator, blue / denominator


def measure(actual: Iterable[Sequence[float]], expected: Iterable[Sequence[float]]) -> Metrics:
    maximum = 0.0
    absolute_sum = 0.0
    squared_sum = 0.0
    component_count = 0
    row_count = 0

    actual_iterator = iter(actual)
    expected_iterator = iter(expected)
    while True:
        try:
            actual_row = next(actual_iterator)
            actual_done = False
        except StopIteration:
            actual_done = True
            actual_row = ()
        try:
            expected_row = next(expected_iterator)
            expected_done = False
        except StopIteration:
            expected_done = True
            expected_row = ()

        if actual_done or expected_done:
            if actual_done != expected_done:
                raise CubeError("sample counts differ")
            break
        if len(actual_row) != 3 or len(expected_row) != 3:
            raise CubeError("metric rows must contain RGB triplets")

        for actual_value, expected_value in zip(actual_row, expected_row):
            difference = abs(float(actual_value) - float(expected_value))
            maximum = max(maximum, difference)
            absolute_sum += difference
            squared_sum += difference * difference
            component_count += 1
        row_count += 1

    if row_count == 0:
        raise CubeError("no samples to compare")
    return Metrics(maximum, absolute_sum / component_count, math.sqrt(squared_sum / component_count))


def print_metrics(metrics: Metrics) -> None:
    print(f"maximum absolute RGB error: {metrics.maximum_absolute:.12g}")
    print(f"mean absolute RGB error:    {metrics.mean_absolute:.12g}")
    print(f"RMS RGB error:              {metrics.rms:.12g}")


def command_inspect(args: argparse.Namespace) -> int:
    cube = load_cube(args.cube)
    print(f"valid CUBE: {args.cube}")
    print(f"LUT size:   {cube.size}^3 ({len(cube.samples):,} RGB rows)")
    print(f"domain:     {cube.domain_min} to {cube.domain_max}")
    return 0


def command_identity(args: argparse.Namespace) -> int:
    cube = load_cube(args.cube)
    metrics = measure(cube.samples, identity_samples(cube.size))
    print_metrics(metrics)
    if metrics.maximum_absolute > args.tolerance:
        print(f"FAIL: maximum error exceeds tolerance {args.tolerance:g}", file=sys.stderr)
        return 1
    print(f"PASS: maximum error is within tolerance {args.tolerance:g}")
    return 0


def command_compare(args: argparse.Namespace) -> int:
    reference = load_cube(args.reference)
    candidate = load_cube(args.candidate)
    if reference.size != candidate.size:
        raise CubeError(f"LUT size mismatch: {reference.size} versus {candidate.size}")
    metrics = measure(candidate.samples, reference.samples)
    print_metrics(metrics)
    if metrics.maximum_absolute > args.tolerance:
        print(f"FAIL: maximum error exceeds tolerance {args.tolerance:g}", file=sys.stderr)
        return 1
    print(f"PASS: maximum error is within tolerance {args.tolerance:g}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect", help="strictly validate structure and print metadata")
    inspect_parser.add_argument("cube", type=pathlib.Path)
    inspect_parser.set_defaults(handler=command_inspect)

    identity_parser = subparsers.add_parser("identity", help="compare a CUBE against the ideal identity lattice")
    identity_parser.add_argument("cube", type=pathlib.Path)
    identity_parser.add_argument("--tolerance", type=float, default=1.0e-6)
    identity_parser.set_defaults(handler=command_identity)

    compare_parser = subparsers.add_parser("compare", help="compare two CUBE files at every lattice node")
    compare_parser.add_argument("reference", type=pathlib.Path)
    compare_parser.add_argument("candidate", type=pathlib.Path)
    compare_parser.add_argument("--tolerance", type=float, default=1.0e-6)
    compare_parser.set_defaults(handler=command_compare)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    tolerance = float(getattr(args, "tolerance", 0.0))
    if not math.isfinite(tolerance) or tolerance < 0.0:
        parser.error("--tolerance must be finite and non-negative")
    try:
        return int(args.handler(args))
    except CubeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
