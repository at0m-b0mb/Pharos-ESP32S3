#!/usr/bin/env python3
"""Check every OUI in the roster's vendor table against the IEEE registry.

A prefix in that table is a CLAIM about who made a device, and the class
beside it turns the claim into a verdict. A wrong prefix is worse than a
missing one: a device absent from the table is reported as unknown, which is
true, while a device matched to the wrong row is reported confidently as
something it is not.

Ten of the first fifty-six entries were wrong when this was first run -
Hikvision and Dahua were each other, two prefixes belonged to Microsoft and
Google while claiming Fitbit and Garmin, one was in no registry at all, and
one had the locally-administered bit set, which means it is a randomised
address and cannot be an assignment.

    python3 tools/check_ouis.py            # downloads the registry
    python3 tools/check_ouis.py oui.csv    # or uses a local copy

Exits non-zero if any entry cannot be confirmed.
"""
import csv, io, os, re, sys, tempfile, urllib.request

REGISTRIES = [
    "https://standards-oui.ieee.org/oui/oui.csv",
    "https://standards-oui.ieee.org/oui28/mam.csv",
    "https://standards-oui.ieee.org/oui36/oui36.csv",
]
ROSTER = os.path.join(os.path.dirname(__file__), "..", "components",
                      "pharos_engine", "pharos_roster.c")

# The registry writes a company's name many ways across decades of filings.
ALIAS = {
    "apple": ["apple"], "espressif": ["espressif"],
    "raspberry pi": ["raspberry pi"], "samsung": ["samsung"],
    "google": ["google"], "amazon": ["amazon"], "intel": ["intel"],
    "tp-link": ["tp-link", "tplink"], "ubiquiti": ["ubiquiti"],
    "netgear": ["netgear"], "roku": ["roku"], "wyze": ["wyze"],
    "hikvision": ["hikvision"], "sonos": ["sonos"], "belkin": ["belkin"],
    "cisco meraki": ["meraki", "cisco"], "axis": ["axis communications"],
    "acti": ["acti"], "dahua": ["dahua"], "withings": ["withings"],
    "canon": ["canon"], "epson": ["epson"], "brother": ["brother"],
    "hp": ["hp", "hewlett"], "fitbit": ["fitbit"], "garmin": ["garmin"],
    "d-link": ["d-link"], "microsoft": ["microsoft"],
}


def load_registry(paths):
    reg = {}
    for path in paths:
        with io.open(path, encoding="utf-8", errors="replace", newline="") as f:
            for row in csv.DictReader(f):
                a = (row.get("Assignment") or "").strip().upper()
                if a:
                    reg.setdefault(a[:6], (row.get("Organization Name") or "").strip())
    return reg


def table_entries(path):
    src = io.open(path, encoding="utf-8").read()
    blk = src[src.index("k_ouis[] = {"):]
    blk = blk[:blk.index("};")]
    return re.findall(
        r"\{\s*\{\s*0x([0-9A-Fa-f]{2}),\s*0x([0-9A-Fa-f]{2}),"
        r"\s*0x([0-9A-Fa-f]{2})\s*\},\s*\"([^\"]+)\",\s*(RD_[A-Z_]+)", blk)


def norm(s):
    """Reduce a company name to its distinguishing words.

    Substring stripping mangled names it was meant to tidy - "Samsung
    Electronics Co.,Ltd" came out as "samsungltd", because removing " co."
    from "co.,ltd" left the rest glued together. Punctuation becomes space
    first, then whole corporate words are dropped.
    """
    s = re.sub(r"[^a-z0-9]+", " ", s.lower())
    drop = {"co", "ltd", "inc", "incorporated", "corporation", "corp",
            "corporate", "technologies", "technology", "electronics", "gmbh",
            "limited", "company", "international", "the", "digital",
            "systems", "system", "industries", "trading", "group", "holdings",
            "america", "usa", "llc", "ag", "sa", "ab", "bv", "nv", "plc"}
    return " ".join(w for w in s.split() if w not in drop)


def main():
    if len(sys.argv) > 1:
        paths = sys.argv[1:]
    else:
        paths = []
        tmp = tempfile.mkdtemp(prefix="pharos-oui-")
        for url in REGISTRIES:
            dest = os.path.join(tmp, os.path.basename(url))
            sys.stderr.write("fetching %s\n" % url)
            urllib.request.urlretrieve(url, dest)
            paths.append(dest)

    reg = load_registry(paths)
    rows = table_entries(ROSTER)
    bad = []
    for a, b, c, vendor, klass in rows:
        oui = (a + b + c).upper()
        if int(a, 16) & 0x02:
            bad.append((oui, vendor, klass,
                        "locally-administered bit set: a randomised address, "
                        "never an assignment"))
            continue
        real = reg.get(oui)
        if real is None:
            bad.append((oui, vendor, klass, "not in any IEEE registry"))
            continue
        names = ALIAS.get(vendor.lower(), [vendor.lower()])
        # Both sides through the same normaliser, then matched as whole
        # words: a short alias like "hp" otherwise matched inside longer
        # names, and an unnormalised one like "tp-link" never matched the
        # registry's "TP-LINK TECHNOLOGIES CO.,LTD." at all.
        rn = norm(real)
        if not any(re.search(r"\b%s\b" % re.escape(norm(n)), rn)
                   for n in names):
            bad.append((oui, vendor, klass, "IEEE says: %s" % real))

    print("%d entries checked, %d confirmed, %d to look at"
          % (len(rows), len(rows) - len(bad), len(bad)))
    for oui, vendor, klass, why in bad:
        print("  %s  table says %-14s %-14s  %s" % (oui, vendor, klass, why))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
