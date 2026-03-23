#!/usr/bin/env python3
"""Append a precompiled AOT binary as a custom section to a WASM file.

Supports both WAMR and WasmEdge AOT formats:

  WAMR:     wamrc -o input.aot input.wasm
            python3 append_aot_to_wasm.py input.wasm input.aot output.wasm

  WasmEdge: wasmedgec input.wasm input_aot.wasm
            python3 append_aot_to_wasm.py input.wasm input_aot.wasm output.wasm --section-name wasmedge-aot

The resulting WASM file contains the original bytecode plus an extra custom
section that the runtime can detect and load as pre-compiled native code,
bypassing the interpreter entirely.

Usage:
    python3 append_aot_to_wasm.py input.wasm input.aot output.wasm [--section-name NAME]

Options:
    --section-name NAME   Custom section name (default: wamr-aot)
"""

import struct
import sys


def encode_uleb128(value: int) -> bytes:
    """Encode an unsigned integer as LEB128."""
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        result.append(byte)
        if not value:
            break
    return bytes(result)


def make_custom_section(name: str, data: bytes, current_offset: int = 0) -> bytes:
    """Build a WASM custom section (id=0) with the given name and payload.
    
    The payload contains the AOT binary padded so that it is properly aligned
    when the WASM file is memory mapped.
    """
    name_bytes = name.encode("utf-8")
    name_len = encode_uleb128(len(name_bytes))

    # Calculate padding to ensure the AOT binary is aligned to a 4-byte boundary
    # inside the final WASM file.
    # The layout of the section is:
    #   [1 byte section_id] + [ULEB128 section_size] + [ULEB128 name_len] + [name_bytes] + [1 byte padding_count] + [padding_count bytes of padding] + [aot_data]
    
    # First, calculate the length of section_size. Since it depends on the total
    # payload size, we might need a couple of iterations to stabilize, but since
    # max padding is 3 bytes, it rarely changes the ULEB128 size.
    # Let's do a safe approximation assuming section_size takes the same number of bytes.
    # Or simply construct the prefix without padding, see where data lands, then pad.
    
    # Let's iterate up to 4 times to find stable padding
    padding_count = 0
    section_size_bytes = b""
    payload = b""
    section_id = b"\x00"
    
    for _ in range(4):
        padded_data = bytes([padding_count]) + (b"\x00" * padding_count) + data
        payload = name_len + name_bytes + padded_data
        section_size_bytes = encode_uleb128(len(payload))
        
        # Calculate offset where the actual data begins
        prefix_len = current_offset + len(section_id) + len(section_size_bytes) + len(name_len) + len(name_bytes) + 1 + padding_count
        
        # We want the data to start at a 4-byte aligned offset
        if prefix_len % 4 == 0:
            break
            
        # Adjust padding for the next iteration if not aligned
        padding_count = (padding_count + (4 - (prefix_len % 4))) % 4

    return section_id + section_size_bytes + payload


def main():
    # Parse arguments
    args = sys.argv[1:]
    section_name = "wamr-aot"

    # Extract --section-name if present
    filtered_args = []
    i = 0
    while i < len(args):
        if args[i] == "--section-name" and i + 1 < len(args):
            section_name = args[i + 1]
            i += 2
        else:
            filtered_args.append(args[i])
            i += 1

    if len(filtered_args) != 3:
        print(f"Usage: {sys.argv[0]} input.wasm input.aot output.wasm [--section-name NAME]",
              file=sys.stderr)
        print(f"\nDefault section name: wamr-aot", file=sys.stderr)
        print(f"For WasmEdge AOT: --section-name wasmedge-aot", file=sys.stderr)
        sys.exit(1)

    wasm_path, aot_path, out_path = filtered_args[0], filtered_args[1], filtered_args[2]

    with open(wasm_path, "rb") as f:
        wasm_data = f.read()

    with open(aot_path, "rb") as f:
        aot_data = f.read()

    # Verify WASM magic number.
    if wasm_data[:4] != b"\x00asm":
        print("Error: input does not appear to be a valid WASM file.",
              file=sys.stderr)
        sys.exit(1)

    custom_section = make_custom_section(section_name, aot_data, current_offset=len(wasm_data))

    # Append the custom section at the end of the WASM binary.
    output = wasm_data + custom_section

    with open(out_path, "wb") as f:
        f.write(output)

    print(f"Wrote {len(output)} bytes to {out_path}")
    print(f"  Original WASM: {len(wasm_data)} bytes")
    print(f"  AOT payload:   {len(aot_data)} bytes")
    print(f"  Custom section: {len(custom_section)} bytes (name='{section_name}')")


if __name__ == "__main__":
    main()
