#!/usr/bin/env python3
"""Offline worktree regression checks, not a credential/history certification.
Only type, count, relative path and match digest are printed; never values.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# Mandatory public upstream copyright contacts are not owner secrets. Accept only
# exact reviewed notice bytes at these paths; no directory-wide exclusion.
REVIEWED_UPSTREAM_NOTICES = {
    "LICENSES/Newlib-notices.txt": "0681089a556e93791da82718d68011ba452de245f7f59c3846936304756ac0c0",
    "LICENSES/Newlib-toolchain-notices.txt": "422aa40293093fb54fc66e692a0d68fd0b24ed5602e5d1d33ad05ba3909057e9",
    "LICENSES/WPA-supplicant-notices.txt": "a87ac4e333d0f120408a9d814e40c3672cd27f365af89b4f2f6631f7a9338953",
}

def reviewed_upstream_notice(name, data):
    return hashlib.sha256(data).hexdigest() == REVIEWED_UPSTREAM_NOTICES.get(name)

RULES = {
    "home_path": rb"/(?:Users|home)/[A-Za-z0-9_.-]+/",
    "private_ip": rb"(?<![\d.])(?:192\.168(?:\.\d{1,3}){2}|10(?:\.\d{1,3}){3}|172\.(?:1[6-9]|2\d|3[01])(?:\.\d{1,3}){2})(?![\d.])",
    "internal_host": rb"\b[A-Za-z0-9_.-]+\.ts\.net\b",
    "private_key": rb"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----",
    "credential_token": rb"(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[A-Z0-9]{16}|sk-(?:proj-)?[A-Za-z0-9_-]{24,}|xox[baprs]-[A-Za-z0-9-]{15,})",
    "configured_credential": rb"(?i)\b(?:password|passwd|ssid|api[_-]?key|secret|access[_-]?token)\s*[:=]\s*[\"'][^\"'\r\n]{1,200}[\"']",
    "device_mac": rb"(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b",
    "email": rb"(?i)\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b",
}

def findings(data):
    hits = []
    for kind, pattern in RULES.items():
        for match in re.finditer(pattern, data):
            value = match.group()
            if kind == "device_mac" and value.lower() in (b"ff:ff:ff:ff:ff:ff", b"00:00:00:00:00:00"):
                continue
            if kind == "email" and value.lower().endswith(b"@users.noreply.github.com"):
                continue
            hits.append((kind, hashlib.sha256(value).hexdigest()))
    return hits

def main():
    result = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=ROOT, check=True, capture_output=True)
    paths = sorted(set(result.stdout.decode().strip("\0").split("\0")))
    issues = []
    notices = []
    for name in paths:
        if not name:
            continue
        path = ROOT / name
        if path.is_symlink():
            issues.append({"type": "symlink_requires_review", "path": name, "count": 1})
            continue
        data = path.read_bytes()
        if reviewed_upstream_notice(name, data):
            notices.append({"path": name, "sha256": hashlib.sha256(data).hexdigest(), "classification": "verbatim public upstream notice"})
            continue
        for kind, digest in findings(data):
            issues.append({"type": kind, "path": name, "count": 1, "sha256": digest})
    print(json.dumps({"files": len(paths), "findings": issues, "reviewed_upstream_notices": notices, "scope": "tracked and nonignored worktree; no history/archive/entropy guarantee"}, indent=2))
    return bool(issues)

if __name__ == "__main__":
    sys.exit(main())
