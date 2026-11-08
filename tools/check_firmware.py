"""Validate recorded research observations; never modifies or installs firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('report', type=Path)
    parser.add_argument('--profile', type=Path, default=Path(__file__).resolve().parents[1] /
                        'firmware/C91C6FC8AA4C39222D6CCFE0FFF468543105E59B.json')
    args = parser.parse_args()
    profile = json.loads(args.profile.read_text())
    report = args.report.read_text()
    matches = re.findall(r'Module \d+ base=([0-9A-F]+) size=([0-9A-F]+) build=([0-9A-F]+)', report)
    matching = [m for m in matches if m[2] == profile['build_id']]
    if len(matching) != 1:
        raise SystemExit('Build ID does not uniquely match this analysis profile.')
    data = args.snapshot.read_bytes()
    if len(data) != profile['module_size'] or int(matching[0][1], 16) != len(data):
        raise SystemExit('Module/snapshot size mismatch.')
    for item in profile['observations']:
        offset = int(item['offset'], 0)
        expected = bytes.fromhex(item['bytes'])
        if data[offset:offset + len(expected)] != expected:
            raise SystemExit(f"Instruction mismatch at {item['offset']}: {item['meaning']}")
        print(f"MATCH {item['offset']}: {item['meaning']}")
    print('Snapshot SHA256:', hashlib.sha256(data).hexdigest())
    print('Analysis matches. This does not validate a sink patch or its ABI.')


if __name__ == '__main__':
    main()
