#!/usr/bin/env python3
"""Turn a Switch profile (lcs_samples.bin) into function names.

    python3 lcs/switch/symbolize.py LCSNative.elf lcs_samples.bin [lcs_log.txt]

LCSNative.elf must come from the same build as the .nro that wrote the samples
(CI keeps it in the LCSNative-map-<version> artifact). The log, if given,
supplies the thread names. Prints, per thread, the functions that took the most
samples and, for the top ones, who called them (from the link register).
"""

from __future__ import annotations

import bisect
import collections
import re
import shutil
import struct
import subprocess
import sys


def read_symbols(path: str) -> tuple[list[int], list[str]]:
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit(f"{path}: not a 64-bit ELF")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    symbols: list[tuple[int, str]] = []
    for sh_name, sh_type, _flags, _addr, offset, size, link, _info, _align, entsize in sections:
        if sh_type != 2:  # SHT_SYMTAB
            continue
        str_offset = sections[link][4]
        for index in range(size // entsize):
            name_off, info, _other, shndx, value, _size = struct.unpack_from(
                "<IBBHQQ", data, offset + index * entsize)
            if (info & 0xF) != 2 or shndx == 0:  # functions only
                continue
            end = data.index(b"\0", str_offset + name_off)
            symbols.append((value, data[str_offset + name_off:end].decode(errors="replace")))
    symbols.sort()
    return [address for address, _ in symbols], [name for _, name in symbols]


def demangle(names: list[str]) -> list[str]:
    tool = shutil.which("c++filt") or shutil.which("aarch64-none-elf-c++filt")
    if not tool or not names:
        return names
    out = subprocess.run([tool], input="\n".join(names), capture_output=True, text=True).stdout
    lines = out.split("\n")
    return lines[:len(names)] if len(lines) >= len(names) else names


def thread_names(log_path: str | None) -> dict[int, str]:
    names: dict[int, str] = {}
    if not log_path:
        return names
    for line in open(log_path, errors="replace"):
        match = re.search(r"\[watch\] threads:(.*)", line)
        if match:
            for item in match.group(1).split():
                index, _, name = item.partition("=")
                if index.isdigit():
                    names[int(index)] = name
    return names


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    addresses, raw_names = read_symbols(sys.argv[1])
    names = demangle(raw_names)
    blob = open(sys.argv[2], "rb").read()
    if not blob.startswith(b"LCSSAMPLES1"):
        raise SystemExit(f"{sys.argv[2]}: not an lcs_samples.bin")
    labels = thread_names(sys.argv[3] if len(sys.argv) > 3 else None)

    def where(offset: int) -> str:
        at = bisect.bisect_right(addresses, offset) - 1
        return names[at] if at >= 0 else f"+{offset:x}"

    by_thread: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    callers: dict[tuple[int, str], collections.Counter] = collections.defaultdict(collections.Counter)
    span = [None, None]
    for offset in range(16, len(blob) - 23, 24):
        time_ms, thread, pc, lr = struct.unpack_from("<IB3xQQ", blob, offset)
        function = where(pc)
        by_thread[thread][function] += 1
        callers[(thread, function)][where(lr)] += 1
        span[0] = time_ms if span[0] is None else min(span[0], time_ms)
        span[1] = time_ms if span[1] is None else max(span[1], time_ms)
    if span[0] is not None:
        print(f"{(len(blob) - 16) // 24} samples over {(span[1] - span[0]) / 1000:.0f} s")
    for thread, counter in sorted(by_thread.items()):
        total = sum(counter.values())
        print(f"\n== thread {thread} {labels.get(thread, '')} ({total} samples)")
        for rank, (function, hits) in enumerate(counter.most_common(25)):
            print(f"{100 * hits / total:6.1f}%  {function}")
            if rank < 6:
                for caller, count in callers[(thread, function)].most_common(3):
                    print(f"           <- {100 * count / hits:4.0f}% {caller}")


if __name__ == "__main__":
    main()
