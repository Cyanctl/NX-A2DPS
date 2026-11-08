"""Read-only AArch64 snapshot inspection. Requires capstone (pip install capstone).

Addresses printed are offsets from the module base, never patch-ready addresses.
The binary snapshot stays local and is not part of the source repository.
"""
import argparse
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '.research/python'))
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('--start', type=lambda x: int(x, 0))
    parser.add_argument('--size', type=lambda x: int(x, 0), default=0x100)
    args = parser.parse_args()
    data = args.snapshot.read_bytes()
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cs.detail = True
    if args.start is not None:
        for ins in cs.disasm(data[args.start:args.start + args.size], args.start):
            print(f'{ins.address:08x}: {ins.mnemonic:8s} {ins.op_str}')
        return
    strings = {}
    for match in re.finditer(rb'[ -~]{6,}\x00', data):
        if any(key in match[0].lower() for key in
               (b'bsa', b'bluetooth', b' av ', b'av_', b'a2dp', b'sink', b'sbc')):
            strings[match.start()] = match[0][:-1].decode()
            print(f'STR {match.start():08x}: {strings[match.start()]}')
    regs = {}
    # Decode each word separately to survive literal pools and embedded data.
    for address in range(0, min(len(data), 0x118000), 4):
        instructions = list(cs.disasm(data[address:address + 4], address))
        if not instructions:
            regs.clear()
            continue
        ins = instructions[0]
        ops = ins.operands
        if ins.mnemonic == 'adrp' and len(ops) == 2 and ops[1].type == ARM64_OP_IMM:
            regs[ops[0].reg] = (ops[1].imm, address)
        elif ins.mnemonic == 'add' and len(ops) == 3 and ops[1].type == ARM64_OP_REG and ops[2].type == ARM64_OP_IMM:
            source = regs.get(ops[1].reg)
            if source and address - source[1] < 40:
                target = source[0] + (ops[2].imm << ops[2].shift.value)
                if target in strings:
                    print(f'REF {address:08x} -> {target:08x}: {strings[target]}')
                regs[ops[0].reg] = (target, address)
        if ins.mnemonic in ('mov', 'movz') and len(ops) == 2 and ops[1].type == ARM64_OP_IMM:
            if ops[1].imm in (0x110a, 0x110b, 0x110d):
                print(f'UUID {address:08x}: {ins.mnemonic} {ins.op_str}')
        if ins.mnemonic in ('ret', 'b', 'br'):
            regs.clear()


if __name__ == '__main__':
    main()
