# Device validation

Date: 2026-11-08. Target: original Switch, HOS 22.5.0, Atmosphère,
MissionControl raw version 0x1000 (0.16.0).

Build 0.5.0 compiled with devkitA64/libnx. Project C files used
`-Wall -Wextra -Werror`; upstream library sources used the portable C path.

On-device checks:

- Bluetooth, BTM, MissionControl, process ID and module metadata queries succeeded.
- RTP/SBC selftest returned PASS, zero failures. Covers encoded/decoded 48 kHz
  joint stereo and 44.1 kHz mono, raw native payload delivery, multiple frames,
  fragmentation, packet loss, duplicates, sequence wrap, RTP CSRC/extension/
  padding, truncated frames, invalid configuration, CRC corruption and 4,096
  deterministic malformed-input smoke cases. No memory sanitizer was run.
- 48 kHz stereo Int16 output buffer submitted, started, played and released.
  This confirms the audio service path; audibility was not independently measured.
- Downloading the installed NRO over USB produced the same SHA-256 as the build:
  `3117878C2E1E37C4CEDCED4FDFF1D40A39E986C396517C4F36A8B91879784A68`.
- Python tools compiled with `py_compile`.
- Firmware observation checker accepted the matching local snapshot/report and
  rejected a local copy with a changed instruction at offset 0x626AC.

The earlier read-only code snapshot copied 1,437,696 bytes into a 3,096,576-byte
module image. Other mappings were zero-filled. Debug attachment and detachment
succeeded. No firmware patch, pairing change, DRM access or receive hook was
installed. Firmware bytes are excluded from version control.

The exact final device log is in `device-report-0.5.0.txt`.

## Native sink updates (v6-v10)

The no-hook statements above describe v5 only. V6 native media/IPC round-trip
and code restoration passed. V7-v9 registered sink SEP/SDP successfully. Android
bonded, configured 44.1 kHz joint-stereo SBC and opened, then disconnected before
START. No live packets arrived. V8 signaling isolation and v9 A2DP 1.2 SDP plus
Android forget/re-pair did not resolve the disconnect. V9 audio pipeline tests
passed and normal sink removal succeeded. Full log: `device-report-0.9.0.txt`.

V10 adds outgoing HCI Disconnect and incoming Disconnect Complete logging.
Both original entry fingerprints match the clean firmware snapshot. Compiled
assembly inspection confirmed preserved arguments and displaced prologues.
Bridge size: 1232 bytes within 1424 verified unused executable bytes. On-device
RTP/SBC and audio pipeline self-tests pass. V10 sink registration and normal
removal also returned success on the console. Live trace is pending.

Unverified: live media reception/playback, sustained streaming, phone
compatibility, controller coexistence while streaming, sleep and forced-exit
recovery. This remains an experimental implementation, not a working
end-to-end A2DP sink.
