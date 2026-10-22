#!/usr/bin/env python3
"""Apply the VA39 (39-bit virtual address space) memory-layout patches to an
upstream Antigravity CLI engine binary.

This is a byte-for-byte equivalent of the inline patcher used by the upstream
Termux ``build.sh``, but implemented on top of ``bytes.find`` so it runs in
seconds instead of minutes on the ~200 MB engine binary.

The upstream engine links Google's TCMalloc, which assumes a 48-bit virtual
address space.  On Android userland (39-bit VA) the aligned-mapping arithmetic
overflows past the addressable range, which surfaces as ``MmapAligned()
failed`` aborts or segmentation faults.  The rewrites below retarget that
arithmetic to a 39-bit layout and stub the ``faccessat2`` wrapper so it stays
available on older kernels.

Usage:
    patch_va39.py <input-binary> <output-binary>
"""

import hashlib
import pathlib
import struct
import sys

# Bitfield-extraction immediate rewrites: (immr, imms) -> (new_immr, new_imms).
UBFX_REWRITE = (42, 44, 35, 37)
LSL_REWRITE = (22, 21, 29, 28)

# Paired instruction rewrite: (first_word, second_word) -> (new_first, new_second).
MASK_REWRITE = (0x92D3800A, 0xF2E0000A, 0x9280000A, 0xD35DFD4A)

# Single instruction rewrite.
MMAP_REWRITE = (0xF2E00029, 0xD3596129)

# Unconditional single instruction rewrites.
WORD_REWRITES = {
    0xD2C20009: 0xD2C00409,
    0xD2C2000A: 0xD2C0040A,
    0xF2C20008: 0xF2DFF408,
    0xF2C20009: 0xF2DFF409,
    0xD2C10009: 0xD2C00209,
    0xD2C1000A: 0xD2C0020A,
    0xF2C38008: 0xF2DFF708,
    0xF2C38009: 0xF2DFF709,
    0x92560A6C: 0x925D0A6C,
    0x92560A6A: 0x925D0A6A,
    0xD2C3000D: 0xD2C0060D,
    0xD2C3000C: 0xD2C0060C,
    0xD2C08008: 0xD2C00108,
}

# The faccessat2 call site loads the syscall number 0x5e (94) into x0; the
# rewrite loads 0 instead so the call degrades to a harmless no-op.
FACCESSAT2_REWRITE = 0xD2800600

# ``(w & 0x7F800000) == 0x53000000`` selects the UBFX family.  The mask leaves
# bit 31 free, so byte 3 of the word is either 0x53 or 0xd3; bit 23 (the top
# bit of byte 2) is pinned to zero, byte 2 otherwise holds the immr field and
# the top six bits of byte 1 hold the imms field.
UBFX_OPCODE_BYTE = 0x53
UBFX_OPCODE_MASK = 0x7F


def _pack(word):
    return struct.pack("<I", word)


def _clear_immediates(word):
    """Clear the immr (bits 21..16) and imms (bits 15..10) fields."""
    return word & ~(0x3F << 16) & 0xFFFFFFFF & ~(0x3F << 10) & 0xFFFFFFFF


def _patch_ubfx_family(data, rule, counts, counter):
    """Rewrite every UBFX-family word whose (immr, imms) matches ``rule``."""
    immr, imms, new_immr, new_imms = rule
    size = len(data)
    word_mask = _clear_immediates(0xFFFFFFFF)

    # Byte 3 of the word carries the UBFX opcode family.  Bit 31 is not part of
    # the opcode mask, so the byte can be 0x53 or 0xd3; both are scanned.
    #
    # Note that the opcode mask also leaves bit 22 of byte 2 unconstrained, so
    # the immr field can appear as either ``immr`` or ``immr | 0x40``.  The
    # whole word is therefore validated instead of matched on a byte signature.
    expected_byte1 = ((imms & 0x3F) << 2) & 0xFC

    for opcode_byte in (UBFX_OPCODE_BYTE, UBFX_OPCODE_BYTE | 0x80):
        needle = bytes([opcode_byte])
        pos = 0
        while True:
            idx = data.find(needle, pos)
            if idx < 0:
                break
            pos = idx + 1
            word_off = idx - 3
            if word_off < 0 or word_off + 4 > size or (word_off % 4) != 0:
                continue
            if (data[word_off + 3] & UBFX_OPCODE_MASK) != UBFX_OPCODE_BYTE:
                continue
            if (data[word_off + 1] & 0xFC) != expected_byte1:
                continue
            word = struct.unpack_from("<I", data, word_off)[0]
            if (word & 0x7F800000) != 0x53000000:
                continue
            if ((word >> 16) & 0x3F) != immr or ((word >> 10) & 0x3F) != imms:
                continue
            new_word = (word & word_mask) | (new_immr << 16) | (new_imms << 10)
            struct.pack_into("<I", data, word_off, new_word)
            counts[counter] += 1


def patch_buffer(data):
    """Patch ``data`` (a bytearray) in place and return the per-rule counters."""
    counts = {
        "ubfx": 0,
        "lsl": 0,
        "mask": 0,
        "mmap": 0,
        "word_rewrites": 0,
        "faccessat2": 0,
    }

    size = len(data)

    # --- UBFX / LSL immediate rewrites -------------------------------------
    _patch_ubfx_family(data, UBFX_REWRITE, counts, "ubfx")
    _patch_ubfx_family(data, LSL_REWRITE, counts, "lsl")

    # --- paired mask rewrite ----------------------------------------------
    needle = _pack(MASK_REWRITE[0]) + _pack(MASK_REWRITE[1])
    replacement = _pack(MASK_REWRITE[2]) + _pack(MASK_REWRITE[3])
    pos = 0
    while True:
        idx = data.find(needle, pos)
        if idx < 0:
            break
        pos = idx + 1
        if (idx % 4) != 0:
            continue
        data[idx:idx + 8] = replacement
        counts["mask"] += 1

    # --- single mmap rewrite ----------------------------------------------
    needle = _pack(MMAP_REWRITE[0])
    replacement = _pack(MMAP_REWRITE[1])
    pos = 0
    while True:
        idx = data.find(needle, pos)
        if idx < 0:
            break
        pos = idx + 1
        if (idx % 4) != 0:
            continue
        data[idx:idx + 4] = replacement
        counts["mmap"] += 1

    # --- unconditional word rewrites --------------------------------------
    for src_word, dst_word in WORD_REWRITES.items():
        needle = _pack(src_word)
        replacement = _pack(dst_word)
        pos = 0
        while True:
            idx = data.find(needle, pos)
            if idx < 0:
                break
            pos = idx + 1
            if (idx % 4) != 0:
                continue
            data[idx:idx + 4] = replacement
            counts["word_rewrites"] += 1

    # --- faccessat2 call-site rewrite --------------------------------------
    # Pattern: mov x29,x0 ; mov x30,x1 ; mov x0,#0x5e ; bl <...>
    needle = _pack(0xAA1F03E5) + _pack(0xAA1F03E6) + _pack(0xD28036E0)
    pos = 0
    while True:
        idx = data.find(needle, pos)
        if idx < 0:
            break
        pos = idx + 1
        if (idx % 4) != 0:
            continue
        if idx + 16 > size:
            continue
        tail = struct.unpack_from("<I", data, idx + 12)[0]
        if (tail & 0xFC000000) != 0x94000000:
            continue
        struct.pack_into("<I", data, idx + 8, FACCESSAT2_REWRITE)
        counts["faccessat2"] += 1

    return counts


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} <input-binary> <output-binary>", file=sys.stderr)
        return 2

    src = pathlib.Path(argv[1])
    dst = pathlib.Path(argv[2])
    if not src.is_file():
        print(f"error: input not found: {src}", file=sys.stderr)
        return 1

    data = bytearray(src.read_bytes())
    counts = patch_buffer(data)
    dst.write_bytes(data)
    dst.chmod(0o755)

    print(
        "Patched parameters: "
        f"ubfx={counts['ubfx']}, lsl={counts['lsl']}, mask={counts['mask']}, "
        f"mmap={counts['mmap']}, faccessat2={counts['faccessat2']}, "
        f"word_rewrites={counts['word_rewrites']}"
    )
    print(f"sha256: {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
