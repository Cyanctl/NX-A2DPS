# Switch A2DP sink research

This project targets the original Switch's built-in Bluetooth radio under
Atmosphère. **Phone-to-Switch Bluetooth streaming is not working yet.**
The current executable is a diagnostic/research application with a tested SBC
media receiver. It does not advertise an audio sink or install firmware patches.

## Implemented and tested on the console

- Bluetooth and MissionControl capability queries; firmware/module identity.
- Local 48 kHz, stereo, signed 16-bit audio output test.
- RTP extraction with CSRC, extension and padding bounds checks.
- SBC decoding at 44.1/48 kHz using the upstream LGPL SBC library.
- SBC frame validation against negotiated configuration, CRC/error rejection,
  fragmentation, sequence wrap, duplicate rejection and loss recovery.
- A separate payload entry point for native AVDTP callbacks that strip RTP.
- ARM64 self-tests, including 4,096 deterministic malformed packets.
- Optional local snapshot of Bluetooth executable/read-only mappings for
  analysis. Writable memory is excluded. The debugger detaches before SD writes.
- MCP controller/screenshot and Windows MTP deployment/retrieval tools.

`sbc_receiver_packet()` accepts complete media RTP packets.
`sbc_receiver_payload()` accepts the SBC payload header and frames, together
with the original RTP sequence number. Both emit PCM through a synchronous
callback at the negotiated rate and channel count. They require an already
configured and started AVDTP session; they do not implement pairing/signaling.
Call `sbc_receiver_finish()` before initializing an existing receiver again.

## Build

Windows PowerShell, devkitA64, libnx and Switch tools:

```powershell
./build.ps1
# Other installation:
./build.ps1 -DevkitPro C:/path/to/devkitpro
```

Output: `dist/switch-audio-probe-v5.nro`. Build objects are retained in `build/`
and can be relinked after changing the library. The complete libsbc sources,
licenses and pinned upstream revision are in `third_party/libsbc/`.

## Deploy and run

Open USB File Transfer (`haze.nro`) on the Switch and connect USB to Windows.
The MTP tool selects exactly one device named Nintendo Switch and its SD Card;
it writes only the specified file and refuses an existing destination.

```powershell
./tools/switch_mtp.ps1 -Action Upload -LocalFile ./dist/switch-audio-probe-v5.nro
```

Exit USB File Transfer, let the homebrew launcher refresh, and launch **Switch
Audio Probe v5**. Wait for its screen to appear before sending another button.

- **A:** quiet half-second local test tone.
- **X:** snapshot Bluetooth code/read-only mappings to the app's SD folder.
- **Y:** launch `sdmc:/switch/haze.nro` for USB file transfer.
- **PLUS:** exit.

The report is `sdmc:/switch/switch-audio-probe/report.txt`. Each launch replaces
that app-owned report; X replaces the app-owned `bluetooth-code.bin` snapshot.

```powershell
./tools/switch_mtp.ps1 -Action Download -RemoteFolder switch/switch-audio-probe `
  -RemoteFile report.txt -LocalFile .research/device-report.txt
python tools/switch_mcp.py frame
python tools/switch_mcp.py press --buttons 0x400
```

The MCP endpoint defaults to the supplied LAN address. This MCP server supports
controller input and screenshots, not arbitrary code execution or file access.

## Firmware work still required

The tested console reports HOS 22.5.0, Atmosphère and MissionControl version
`0x00001000` (0.16.0). Its Bluetooth module build ID is:

`C91C6FC8AA4C39222D6CCFE0FFF468543105E59B000000000000000000000000`.

The local snapshot identifies a source registration path that explicitly passes
UUID `0x110A` to the SDP/advertisement helpers. It also contains an incoming media
path that calls a callback from the stream configuration at offset `+0x80` after
removing RTP. Its callback receives a stream handle, a buffer, timestamp and
payload type; the RTP sequence is written to the buffer's metadata. These are
disassembly observations, not a fully verified callable ABI.

The version-specific observations in `firmware/` can be checked without writing
anything to the console:

```powershell
python tools/check_firmware.py .research/bluetooth-code.bin .research/report-v3.txt
# Optional disassembly helper (requires capstone):
python tools/inspect_bluetooth.py .research/bluetooth-code.bin --start 0x94c9c --size 0x50
```

The missing implementation is a Bluetooth-process extension that:

1. Registers a sink SEP and SDP UUID `0x110B`, with verified control/data callback
   layouts and codec capabilities; changing the existing UUID alone is inadequate.
2. Handles pairing consent and incoming AVDTP configuration/start/suspend/close.
3. Copies media into a bounded queue and transfers it to the homebrew process
   through a dedicated IPC interface, without consuming system/controller events.
4. Supplies negotiated format and sequence metadata to the existing decoder.
5. Buffers/resamples 44.1 kHz output to Horizon's 48 kHz renderer, recovers after
   disconnect/sleep, and restores previous state on shutdown.
6. Verifies the exact module build and original instructions before any patch,
   preserves MissionControl's existing hooks, and supports removal/recovery.

No receive hook has been installed. Existing source audio/controller behavior
has not been modified. The snapshot and all firmware bytes remain in ignored
local research storage; the repository contains only our observations and code.

## Audio and project boundaries

The intended input is ordinary A2DP audio emitted by the phone. Spotify or
another player continues running on the phone. This code contains no Spotify
authentication, downloads, DRM decryption, or extraction of protected media.

## References

- [libnx Bluetooth API](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/btdrv.h)
- [MissionControl extension interface](https://github.com/ndeadly/MissionControl/blob/master/mc_mitm/source/mc/mc_service.hpp)
- [Bluetooth SBC library](https://git.kernel.org/pub/scm/bluetooth/sbc.git)
- [BlueZ AVDTP implementation](https://github.com/bluez/bluez/blob/master/profiles/audio/avdtp.c)
- [Bluedroid AV registration reference](https://github.com/LineageOS/android_system_bt/blob/lineage-15.1/bta/av/bta_av_main.cc)

Reference sources were used to understand APIs and compare the disassembly.
No Nintendo SDK or Nintendo executable code is included in this repository.
