# Security and privacy

Report suspected vulnerabilities privately through GitHub:
https://github.com/highdrone/esp32-thermal-camera/security/advisories/new

Private vulnerability reporting was enabled when this policy was added. If the
private form becomes unavailable, do not publish credentials or identifying
captures in issues; wait for a private reporting route to become available.

Include the affected source revision, dependency versions and a minimal
redacted reproduction. Use synthetic data. Do not attach credentials, device
flash dumps, real thermal images, personal paths or sensor IDs. Never post a
live credential to demonstrate a finding. Revoke/rotate exposed credentials
through the service owner's secure administration path first; deleting a file
or rewriting history does not invalidate a credential or undo public exposure.

The actively maintained source on `main` is the review target. There is no
promised response SLA, long-term support, certified security boundary or safety
rating. The retained legacy binary has unverified source provenance; rebuilding
is preferred. Old commits and distributed copies are not sanitized by a new
commit.

## Device privacy boundaries

- `WIRELESS_SCREEN` defaults to `true` for compatibility. ESP-NOW broadcasts are
  unencrypted and unauthenticated. A nearby listener can receive thermal data;
  another transmitter can impersonate a screen or camera. The first-camera
  selection is not secure pairing. A wireless screen in battery standby wakes
  every few seconds to broadcast a probe, and turns on for any nearby device that
  answers like a camera; a camera in standby turns on for any nearby device that
  says hello like a screen. Either can also run down the battery. Set
  `WIRELESS_SCREEN false` to disable this firmware's radio link; do not use the
  link for sensitive scenes.
- There is no router password, cloud account, Internet upload, telemetry service
  or remote firmware updater in the application source. Dependency behavior
  and future changes still require review.
- BMP/CSV captures remain on removable microSD. They are not encrypted or
  automatically erased; manage and wipe them yourself before sharing hardware.
- Sensor serial-number and temperature-statistic logging are disabled in current
  source by default. Enabling the debug options exposes those values over USB
  serial. Redact logs before sharing.

This is a hobby thermal imager, not a medical instrument, fire alarm, electrical
safety detector or other life/safety-critical system. Never use its readings as
the sole basis for a health or safety decision.
