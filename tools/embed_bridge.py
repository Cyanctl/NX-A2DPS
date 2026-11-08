"""Embed our compiled Bluetooth-process bridge and its symbol offsets."""
import argparse
from pathlib import Path
import subprocess

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('devkitpro', type=Path)
    args = parser.parse_args()
    tools = args.devkitpro / 'devkitA64/bin'
    subprocess.run([str(tools / 'aarch64-none-elf-objcopy.exe'), '-O', 'binary',
                    'build/bridge.elf', 'build/bridge.bin'], check=True)
    output = subprocess.check_output([str(tools / 'aarch64-none-elf-nm.exe'),
                                      '--defined-only', 'build/bridge.elf'], text=True)
    symbols = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    data = Path('build/bridge.bin').read_bytes()
    if not data or len(data) > 0x590:
        raise SystemExit('Bridge size exceeds verified executable padding')
    header = '#pragma once\n#include <stdint.h>\n'
    for name in ('bridge_entry', 'bridge_state', 'bridge_media', 'bridge_control',
                 'bridge_disconnect_send', 'bridge_disconnect_receive'):
        header += f'#define {name.upper()}_OFFSET 0x{symbols[name]:X}u\n'
    header += 'static const uint8_t bridge_blob[] = {\n'
    for i in range(0, len(data), 16):
        header += '    ' + ','.join(f'0x{x:02x}' for x in data[i:i+16]) + ',\n'
    header += '};\n'
    Path('build/bridge_blob.h').write_text(header)
    print(f'Embedded native bridge: {len(data)} bytes')

if __name__ == '__main__':
    main()
