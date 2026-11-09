# Tests

Everything here runs on a laptop or in CI. None of it needs a Cardputer.

| what | run | checks |
|------|-----|--------|
| Connector | `node --test connector/tests/*.test.mjs` | The Node connector against a mock relay and a fake agent. |
| Firmware logic | `tests/host/run.sh` | The firmware's own headers (sessions, phone setup, agent protocol, notes, approvals and more) compiled for this machine with sanitizers on. |
| Protocol interop | `tests/interop/run.sh` | The connector against the firmware's protocol code, both ways. |
| Screens | `tests/ui/preview.sh` | Every screen rendered from the real UI code to `tests/build/ui/out/`, with redraw checks and the setup QR codes read back. |

The last three need `g++`, OpenSSL headers, `node`, `python3` with Pillow, and the libraries PlatformIO fetches. Run
`pio run -e cardputer-adv` (or `pio pkg install -e cardputer-adv`) once first.

Two mutation runners check that the tests can fail. Each breaks one behavior at a time and expects a test to notice:
`node connector/tests/mutate.mjs` and `python3 tests/host/mutate.py`. They take several minutes and are not part of CI.

`tests/host/shim.h` stands in for Arduino (`String`, `SD`, `Preferences`) on the host. Audio, Wi-Fi and the keyboard
are not covered here and are checked on the device.
