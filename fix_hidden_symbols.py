#!/usr/bin/env python3
"""Patch ELF object file to change HIDDEN visibility to DEFAULT for all GLOBAL symbols.

In Android's ART assembly files, the ENTRY macro uses .hidden to avoid PLT calls
in shared libraries. For static builds, this causes the linker to demote GLOBAL
symbols to LOCAL, making them invisible to other object files that reference them.

This script patches the st_other field of each GLOBAL FUNC/NOTYPE symbol in .symtab,
changing STV_HIDDEN (2) to STV_DEFAULT (0).

Usage: python3 fix_hidden_symbols.py <input.o> [output.o]
       If output.o is not specified, input.o is modified in-place.
"""

import struct
import sys

# ELF constants
STV_DEFAULT = 0
STV_HIDDEN = 2
STB_GLOBAL = 1
STB_WEAK = 2

def patch_elf_hidden(inpath, outpath=None):
    with open(inpath, 'rb') as f:
        data = bytearray(f.read())

    if outpath is None:
        outpath = inpath

    # Parse ELF header
    if data[:4] != b'\x7fELF':
        print(f"Error: {inpath} is not an ELF file", file=sys.stderr)
        return 1

    ei_class = data[4]  # 1 = 32-bit, 2 = 64-bit
    ei_data = data[5]   # 1 = little-endian, 2 = big-endian

    if ei_data == 1:
        endian = '<'
    else:
        endian = '>'

    if ei_class == 2:
        # 64-bit ELF
        e_shoff = struct.unpack_from(endian + 'Q', data, 40)[0]
        e_shentsize = struct.unpack_from(endian + 'H', data, 58)[0]
        e_shnum = struct.unpack_from(endian + 'H', data, 60)[0]
        e_shstrndx = struct.unpack_from(endian + 'H', data, 62)[0]
    else:
        # 32-bit ELF
        e_shoff = struct.unpack_from(endian + 'I', data, 32)[0]
        e_shentsize = struct.unpack_from(endian + 'H', data, 46)[0]
        e_shnum = struct.unpack_from(endian + 'H', data, 48)[0]
        e_shstrndx = struct.unpack_from(endian + 'H', data, 50)[0]

    # Find .symtab section
    symtab_off = 0
    symtab_size = 0
    symtab_entsize = 0

    for i in range(e_shnum):
        sh_off = e_shoff + i * e_shentsize
        if ei_class == 2:
            sh_type = struct.unpack_from(endian + 'I', data, sh_off + 4)[0]
            sh_offset = struct.unpack_from(endian + 'Q', data, sh_off + 24)[0]
            sh_size = struct.unpack_from(endian + 'Q', data, sh_off + 32)[0]
            sh_entsize = struct.unpack_from(endian + 'Q', data, sh_off + 56)[0]
        else:
            sh_type = struct.unpack_from(endian + 'I', data, sh_off + 4)[0]
            sh_offset = struct.unpack_from(endian + 'I', data, sh_off + 16)[0]
            sh_size = struct.unpack_from(endian + 'I', data, sh_off + 20)[0]
            sh_entsize = struct.unpack_from(endian + 'I', data, sh_off + 36)[0]

        if sh_type == 2:  # SHT_SYMTAB
            symtab_off = sh_offset
            symtab_size = sh_size
            symtab_entsize = sh_entsize
            break

    if symtab_off == 0:
        print(f"Error: no .symtab found in {inpath}", file=sys.stderr)
        return 1

    num_syms = symtab_size // symtab_entsize
    patched = 0

    for i in range(num_syms):
        sym_off = symtab_off + i * symtab_entsize

        if ei_class == 2:
            # Elf64_Sym: st_name(4) st_info(1) st_other(1) st_shndx(2) st_value(8) st_size(8)
            st_info = data[sym_off + 4]
            st_other = data[sym_off + 5]
        else:
            # Elf32_Sym: st_name(4) st_value(4) st_size(4) st_info(1) st_other(1) st_shndx(2)
            st_info = data[sym_off + 12]
            st_other = data[sym_off + 13]

        binding = st_info >> 4
        visibility = st_other & 0x3

        # Change HIDDEN to DEFAULT for GLOBAL and WEAK symbols
        if visibility == STV_HIDDEN and binding in (STB_GLOBAL, STB_WEAK):
            new_other = (st_other & ~0x3) | STV_DEFAULT
            if ei_class == 2:
                data[sym_off + 5] = new_other
            else:
                data[sym_off + 13] = new_other
            patched += 1

    with open(outpath, 'wb') as f:
        f.write(data)

    print(f"Patched {patched} symbols from HIDDEN to DEFAULT in {outpath}")
    return 0

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <input.o> [output.o]", file=sys.stderr)
        sys.exit(1)

    inpath = sys.argv[1]
    outpath = sys.argv[2] if len(sys.argv) > 2 else None
    sys.exit(patch_elf_hidden(inpath, outpath))
