#!/usr/bin/env python3
"""Generate include/base/text_width_table.hpp from Python's unicodedata."""

import argparse
import datetime
import os
import sys
import unicodedata
from pathlib import Path

DEFAULT_OUT = Path(__file__).resolve().parent / "include" / "base" / "text_width_table.hpp"
PER_LINE = 3


def ranges_for(pred):
    runs = []
    for cp in range(0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        if pred(cp):
            if runs and cp == runs[-1][1] + 1:
                runs[-1][1] = cp
            else:
                runs.append([cp, cp])
    return runs


def zero_pred(cp):
    return unicodedata.category(chr(cp)) in ("Mn", "Me", "Cf", "Cc", "Zl", "Zp")


def wide_pred(cp):
    return unicodedata.east_asian_width(chr(cp)) in ("W", "F")


def build_tables():
    zero = ranges_for(zero_pred)
    zero_set = set()
    for lo, hi in zero:
        zero_set.update(range(lo, hi + 1))
    wide = ranges_for(lambda cp: wide_pred(cp) and cp not in zero_set)
    return wide, zero


def emit_ranges(name, table):
    items = [f"{{0x{lo:04X}, 0x{hi:04X}}}," for lo, hi in table]
    width = max((len(item) for item in items), default=0)

    lines = [f"inline constexpr Range {name}[] = {{"]
    for i in range(0, len(items), PER_LINE):
        chunk = items[i:i + PER_LINE]
        row = " ".join(item.ljust(width) for item in chunk).rstrip()
        lines.append("    " + row)
    lines.append("};")
    return "\n".join(lines)


def render(wide, zero, script_name):
    version = unicodedata.unidata_version
    banner = [
        "// -- include/base/text_width_table.hpp",
        "//",
        "// Auto-generated file. Do not edit by hand.",
        f"// Generator: {script_name}",
        f"// Unicode version: {version}",
        "// Codepoint range: U+0000 .. U+10FFFF (skipping U+D800..U+DFFF surrogates)",
        f"// kWide ranges: {len(wide)}  (East Asian Width W/F, excluding zero-width)",
        f"// kZero ranges: {len(zero)}  (Mn/Me/Cf/Cc/Zl/Zp)",
    ]

    return "\n".join(banner) + f"""

#ifndef TEXT_WIDTH_TABLE_HPP
#define TEXT_WIDTH_TABLE_HPP

#include <cstddef>
#include <cstdint>
namespace coding {{
namespace text {{
namespace width_table {{

struct Range {{
  std::uint32_t lo;
  std::uint32_t hi;
}};

inline constexpr char kUnicodeVersion[] = "{version}";

inline constexpr std::size_t kWideRangeCount = {len(wide)};
inline constexpr std::size_t kZeroRangeCount = {len(zero)};

{emit_ranges("kWide", wide)}

{emit_ranges("kZero", zero)}

static_assert(sizeof(kWide) / sizeof(kWide[0]) == kWideRangeCount,
              "kWide range count mismatch");
static_assert(sizeof(kZero) / sizeof(kZero[0]) == kZeroRangeCount,
              "kZero range count mismatch");

}} // namespace width_table
}} // namespace text
}} // namespace coding

#endif // TEXT_WIDTH_TABLE_HPP
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--out", type=Path, default=DEFAULT_OUT,
                    help=f"output path (default: {DEFAULT_OUT})")
    ap.add_argument("--check", action="store_true",
                    help="only verify the existing file is up to date")
    ap.add_argument("--stamp", action="store_true",
                    help="include a generation timestamp "
                         "(respects SOURCE_DATE_EPOCH)")
    args = ap.parse_args()

    wide, zero = build_tables()
    print(f"unicode {unicodedata.unidata_version}: "
          f"wide={len(wide)} ranges, zero={len(zero)} ranges")

    header = render(wide, zero, Path(__file__).name)

    if args.check:
        if not args.out.exists():
            print(f"missing: {args.out}", file=sys.stderr)
            return 1
        if args.out.read_text(encoding="utf-8") != header:
            print(f"stale: {args.out}", file=sys.stderr)
            return 1
        print(f"ok: {args.out}")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(header, encoding="utf-8")
    print(f"wrote {args.out} ({len(header.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())