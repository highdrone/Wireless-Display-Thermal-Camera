"""Privacy scan of a firmware image: tools/privacy_check.py rules over the raw
bytes, printable strings and UTF-16 views, plus local path markers. Prints
counts and kinds only, never the matched values. A clean result is a
heuristic, not proof of absence.

Usage: python3 tools/release/scan_image.py IMAGE [EXTRA_MARKER ...]
Add your own user name or other private strings as extra markers.
"""
import importlib.util, re, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("privacy", ROOT / "tools/privacy_check.py"); privacy = importlib.util.module_from_spec(spec); spec.loader.exec_module(privacy)
data = open(sys.argv[1], "rb").read()
views = {"raw": data}
for enc in ("utf-16-le", "utf-16-be"):
    for shift in (0, 1):
        views[f"{enc}+{shift}"] = data[shift:].decode(enc, errors="replace").encode("utf-8", errors="replace")
printable = b"\n".join(re.findall(rb"[\x20-\x7e]{4,}", data)); views["printable"] = printable
extra = [rb"/Users/", rb"/home/", rb"/root/", rb"/tmp/", rb"/opt/homebrew", rb"C:\Users"] + [m.encode() for m in sys.argv[2:] if m != "-v"]
for name, view in views.items():
    hits = privacy.findings(view)
    more = [p.decode() for p in extra if p.lower() in view.lower()]
    print(f"{name:14s} privacy findings: {len(hits)} {sorted(set(k for k, _ in hits))}  local markers: {more}")
paths = sorted(set(re.findall(rb"/[A-Za-z0-9_.+-]+(?:/[A-Za-z0-9_.+-]+){2,}", data)))
print("absolute-path-like strings:", len(paths), "(list them with -v)")
if "-v" in sys.argv: [print("  ", p.decode()) for p in paths]
