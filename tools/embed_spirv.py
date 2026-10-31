#!/usr/bin/env python3
"""Embed a SPIR-V binary as a C++ header.

The Vulkan application embeds its compute shaders as `uint32_t` arrays so the
executable has no runtime shader files. Regenerate the header with:

  dxc -T cs_6_0 -E CSBake -spirv -fvk-use-dx-layout -DEXPLOSION_VULKAN \
      -I assets/shaders -Fo build/spirv/volume_bake_CSBake.spv \
      assets/shaders/volume_bake.hlsl
  python tools/embed_spirv.py build/spirv/volume_bake_CSBake.spv \
      src/app/VolumeBakeShader.h kVolumeBakeCS

The output format matches the other generated headers: eight words per line,
a `static const size_t <symbol>Size = sizeof(<symbol>);` trailer, and no
dependencies beyond <cstddef>/<cstdint>.
"""

import struct
import sys

WORDS_PER_LINE = 8


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    binary, header, symbol = sys.argv[1:4]

    with open(binary, "rb") as handle:
        data = handle.read()
    if not data or len(data) % 4 != 0:
        print(f"{binary}: not a SPIR-V binary", file=sys.stderr)
        return 1
    if data[:4] != b"\x03\x02\x23\x07":
        print(f"{binary}: missing SPIR-V magic", file=sys.stderr)
        return 1

    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = []
    for start in range(0, len(words), WORDS_PER_LINE):
        chunk = words[start:start + WORDS_PER_LINE]
        lines.append("    " + ", ".join(f"0x{word:08x}u" for word in chunk) + ",")

    text = "\n".join(
        [
            "#pragma once",
            "",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            f"static const uint32_t {symbol}[] = {{",
            *lines,
            "};",
            f"static const size_t {symbol}Size = sizeof({symbol});",
            "",
        ]
    )
    with open(header, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print(f"{header}: {len(words)} words")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
