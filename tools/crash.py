#!/usr/bin/env python3
"""Names the addresses in a crash report (logs/crash.txt from the console).

Usage: tools/crash.py <crash.txt> [OunceChiaki.elf]
The ELF must be from the same build as the NRO that crashed. Runs nm and
addr2line in the build image when they aren't installed locally.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def tool(name, *args):
    exe = f"aarch64-none-elf-{name}"
    if shutil.which(exe):
        return subprocess.check_output([exe, *args], text=True)
    return subprocess.check_output(["docker", "run", "--rm", "-v", f"{ROOT}:/repo", "-w", "/repo",
                                    "ounce-chiaki-build", exe, *args], text=True)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    report = Path(sys.argv[1]).read_text()
    elf = Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "build/switch2/OunceChiaki.elf").resolve()
    rel = elf.relative_to(ROOT) if elf.is_relative_to(ROOT) else elf
    anchor_elf = next(int(l.split()[0], 16) for l in tool("nm", str(rel)).splitlines()
                      if l.endswith(" ounce_crash_anchor"))
    for block in report.strip().split("\n\n"):
        print(block)
        m = re.search(r"anchor 0x([0-9a-f]+)", block)
        if not m:
            continue
        base = int(m.group(1), 16) - anchor_elf
        addrs = re.findall(r"(?:pc|lr) 0x([0-9a-f]+)", block)
        bt = re.search(r"backtrace(.*)", block)
        if bt:
            addrs += re.findall(r"0x([0-9a-f]+)", bt.group(1))
        offsets = [f"0x{int(a, 16) - base:x}" for a in addrs if int(a, 16) >= base]
        if offsets:
            print(f"--- load base 0x{base:x}")
            out = tool("addr2line", "-Cfpie", str(rel), *offsets)
            for off, line in zip(offsets, out.splitlines()):
                print(f"  {off}  {line}")
        print()


if __name__ == "__main__":
    main()
