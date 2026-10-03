# Contributing

Use focused pull requests against `main`. Preserve the existing default wiring,
SH8601/FT3168 V1 and CO5300/CST820 V2 support, camera/viewer controls, USB versus
battery shutdown behavior, and wireless-screen wire format unless a change is
explicitly explained. Never copy credentials, personal configuration, device
captures, flash dumps, build logs, private paths or identifying image metadata
into commits or issues. Use synthetic thermal images for examples.

Before sending a patch, run from the repository root:

```sh
python3 tools/privacy_check.py
python3 -m unittest discover -s tests -v
git diff --check
```

The native tests require Python 3.9+ and a C++11 compiler with AddressSanitizer and
UndefinedBehaviorSanitizer (Clang or GCC). They test malformed BMP headers,
palette endpoints, default configuration, serial privacy, ignore rules and the
legacy artifact checksum. They do not simulate the sensor, display, ESP-NOW,
battery, SD card or power chip. CI performs these offline checks only; it does
not certify an ESP32 compile or hardware behavior.

Follow the pinned build recipe in README. Record the source commit, exact core
and library versions, complete FQBN/options and result in your PR. Do not commit
build outputs. Report V1 and V2 hardware results separately; a successful compile
is not a hardware test. Do not flash any device without its owner's permission.

Keep the root MIT notice and all third-party notices. The bundled Melexis driver
remains Apache-2.0; mark modifications to its files. Contributions to original
project code are under MIT. Security issues belong in the private channel in
SECURITY.md, not a public issue containing a reproduction secret or dump.
