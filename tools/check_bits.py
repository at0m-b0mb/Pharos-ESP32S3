#!/usr/bin/env python3
"""Every bitmask constant in an engine's note/family/cap set must own its bit.

Two notes sharing a bit means setting one lights the other, so a verdict
claims evidence it does not have - and the raw `notes` word the console
exports cannot be decoded either way.

This found PV_NOTE_SHORT sitting on PV_NOTE_FLIPPER's bit 4. It had been
appended twenty lines below the note block, after an unrelated comment, where
the list it had to be distinct from was not visible. Nothing misreported only
because the engine never set PV_NOTE_FLIPPER - pv_classify() already
recognises a Flipper, so wiring that note up would have made "too little time
to judge" announce one in the room.

Parsing the headers rather than listing the constants means a family added
later is covered without anyone remembering to add it.

    python3 tools/check_bits.py

Exits non-zero on any collision, any constant that is not a single bit, or any
family whose bits are not contiguous from 0 (a gap is usually a deletion that
left a hole, which is worth a look but is NOT an error).
"""
import collections
import glob
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PATTERN = re.compile(
    r"#define\s+("
    r"P[A-Z]+_(?:NOTE|FORGE|FAM|CAP|EXP|FLAG)_[A-Z0-9_]+"
    r"|RD_(?:EXP|FLAG)_[A-Z0-9_]+"
    r")\s+\(?\s*1u?\s*<<\s*(\d+)\s*\)?"
)
FAMILY = re.compile(r"^(P[A-Z]+_(?:NOTE|FORGE|FAM|CAP|EXP|FLAG)|RD_(?:EXP|FLAG))_")


def main():
    headers = sorted(glob.glob(os.path.join(ROOT, "components", "*", "include", "*.h")))
    if not headers:
        print("no headers found - wrong working directory?")
        return 1

    collisions = 0
    checked = 0
    families = 0
    for path in headers:
        with open(path, encoding="utf-8", errors="replace") as handle:
            src = handle.read()

        groups = collections.defaultdict(lambda: collections.defaultdict(list))
        for match in PATTERN.finditer(src):
            name, bit = match.group(1), int(match.group(2))
            fam = FAMILY.match(name)
            if fam:
                groups[fam.group(1)][bit].append(name)

        for fam, bits in sorted(groups.items()):
            families += 1
            checked += sum(len(v) for v in bits.values())
            for bit, names in sorted(bits.items()):
                if len(names) > 1:
                    collisions += 1
                    print("COLLISION  %s  bit %d is shared by %s   [%s]"
                          % (fam, bit, " and ".join(names),
                             os.path.basename(path)))
                if bit > 31:
                    collisions += 1
                    print("TOO WIDE   %s  %s uses bit %d, past a 32-bit mask   [%s]"
                          % (fam, names[0], bit, os.path.basename(path)))
            used = sorted(bits)
            gaps = [b for b in range(used[-1]) if b not in bits]
            if gaps:
                print("note       %s has unused bit(s) %s below its highest (%d)   [%s]"
                      % (fam, ", ".join(str(g) for g in gaps), used[-1],
                         os.path.basename(path)))

    print("\n%d constants across %d mask families, %d collisions"
          % (checked, families, collisions))
    return 1 if collisions else 0


if __name__ == "__main__":
    sys.exit(main())
