#!/usr/bin/env python3
"""Analyze an external power-meter CSV without third-party packages."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from statistics import fmean


def normalize(value: str) -> str:
    return "".join(ch.lower() for ch in value.strip() if ch not in " _-()/[]")


def pick_column(fieldnames: list[str], explicit: str | None, candidates: tuple[str, ...]) -> str | None:
    if explicit:
        if explicit not in fieldnames:
            raise ValueError(f"Column not found: {explicit}")
        return explicit
    normalized = {normalize(name): name for name in fieldnames}
    for candidate in candidates:
        if normalize(candidate) in normalized:
            return normalized[normalize(candidate)]
    return None


def parse_number(text: str | None) -> float | None:
    if text is None:
        return None
    cleaned = text.strip().replace(",", "")
    if not cleaned:
        return None
    try:
        value = float(cleaned)
    except ValueError:
        return None
    return value if math.isfinite(value) else None


def main() -> int:
    parser = argparse.ArgumentParser(description="Calculate Pavg/Pmax from a calibrated power-meter CSV")
    parser.add_argument("csv_file", type=Path)
    parser.add_argument("--out", type=Path, default=Path("power_summary.json"))
    parser.add_argument("--power-column")
    parser.add_argument("--voltage-column")
    parser.add_argument("--current-column")
    parser.add_argument("--time-column")
    parser.add_argument("--limit-w", type=float, default=8.8)
    args = parser.parse_args()

    with args.csv_file.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise ValueError("CSV has no header row")
        fieldnames = list(reader.fieldnames)
        power_col = pick_column(fieldnames, args.power_column, ("power_w", "power", "watt", "watts", "功率", "功率w"))
        voltage_col = pick_column(fieldnames, args.voltage_column, ("voltage_v", "voltage", "volt", "电压", "电压v"))
        current_col = pick_column(fieldnames, args.current_column, ("current_a", "current", "amp", "amps", "电流", "电流a"))
        time_col = pick_column(fieldnames, args.time_column, ("time_s", "time", "timestamp", "elapsed_s", "时间", "时间s"))

        if not power_col and not (voltage_col and current_col):
            raise ValueError("Need a power column, or both voltage and current columns")

        powers: list[float] = []
        times: list[float] = []
        skipped = 0
        for row in reader:
            if power_col:
                power = parse_number(row.get(power_col))
            else:
                voltage = parse_number(row.get(voltage_col))
                current = parse_number(row.get(current_col))
                power = voltage * current if voltage is not None and current is not None else None
            if power is None:
                skipped += 1
                continue
            powers.append(power)
            if time_col:
                timestamp = parse_number(row.get(time_col))
                if timestamp is not None:
                    times.append(timestamp)

    if not powers:
        raise ValueError("No valid power samples were found")

    duration = max(times) - min(times) if len(times) >= 2 else None
    pmax = max(powers)
    summary = {
        "source_csv": str(args.csv_file.resolve()),
        "samples": len(powers),
        "skipped_rows": skipped,
        "power_source": power_col or f"{voltage_col} * {current_col}",
        "duration_s": duration,
        "pavg_w": fmean(powers),
        "pmax_w": pmax,
        "limit_w": args.limit_w,
        "meets_pmax_limit": pmax < args.limit_w,
        "comparison": "strictly_less_than",
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
