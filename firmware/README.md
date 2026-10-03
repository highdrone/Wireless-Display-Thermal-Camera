# Legacy firmware image

`manifest.json` identifies the existing merged flash image by its exact SHA-256,
length and **0x0** flash offset. It contains bootloader/partition/application data;
it is not a standalone application image for 0x10000. Never choose an offset based
only on the `.bin` extension.

**Source provenance is unverified.** The current source has privacy and parser fixes
that are not asserted to exist in this legacy binary. A matching checksum proves
byte identity, not safety, source correspondence or hardware behavior. Prefer
building the current source. Do not redistribute newly generated firmware until
it has a recorded source commit, exact dependency versions, build command,
partition/offset information, checksum and binary privacy/license review.

Verify existing bytes offline from the repository root:

```sh
python3 -m unittest discover -s tests -v
```

No firmware was flashed as part of source/privacy checks. Removing a binary from
the current tree does not remove older copies from Git history or releases.
