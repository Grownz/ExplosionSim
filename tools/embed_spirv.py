#!/usr/bin/env python3
"""Embed SPIR-V binaries as a C++ header.

The Vulkan application embeds its compute shaders as `uint32_t` arrays so the
executable has no runtime shader files. Regenerate a header after editing the
HLSL with the same DXC flags the build uses:

  dxc -T cs_6_0 -E CSBake -spirv -fvk-use-dx-layout -DEXPLOSION_VULKAN \
      -I assets/shaders -Fo build/spirv/volume_bake_CSBake.spv \
      assets/shaders/volume_bake.hlsl
  python tools/embed_spirv.py src/app/VolumeBakeShader.h \
      kVolumeBakeCS=build/spirv/volume_bake_CSBake.spv

Headers with several entry points (the Euler sweeps) take one
`symbol=binary.spv` pair per entry point:

  python tools/embed_spirv.py src/app/EulerSweepShader.h \
      kEulerCSSweepX=build/spirv/euler_sweep_CSSweepX.spv \
      kEulerCSSweepY=build/spirv/euler_sweep_CSSweepY.spv ...

The output format matches the other generated headers: eight words per line,
a `static const size_t <symbol>Size = sizeof(<symbol>);` trailer per array, and
no dependencies beyond <cstddef>/<cstdint>.
"""

import struct
import sys

WORDS_PER_LINE = 8
MAGIC = b"\x03\x02\x23\x07"


def read_words(path: str):
    with open(path, "rb") as handle:
        data = handle.read()
    if not data or len(data) % 4 != 0 or data[:4] != MAGIC:
        raise ValueError(f"{path}: not a SPIR-V binary")
    return struct.unpack(f"<{len(data) // 4}I", data)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    header = sys.argv[1]
    entries = []
    for argument in sys.argv[2:]:
        symbol, separator, binary = argument.partition("=")
        if not separator or not symbol or not binary:
            print(f"expected symbol=binary.spv, got {argument!r}",
                  file=sys.stderr)
            return 2
        entries.append((symbol, binary))

    blocks = []
    for symbol, binary in entries:
        try:
            words = read_words(binary)
        except ValueError as error:
            print(error, file=sys.stderr)
            return 1
        lines = []
        for start in range(0, len(words), WORDS_PER_LINE):
            chunk = words[start:start + WORDS_PER_LINE]
            lines.append("    " +
                         ", ".join(f"0x{word:08x}u" for word in chunk) + ",")
        blocks.append(
            "\n".join(
                [
                    f"static const uint32_t {symbol}[] = {{",
                    *lines,
                    "};",
                    f"static const size_t {symbol}Size = sizeof({symbol});",
                ]
            )
        )

    text = "\n".join(
        [
            "#pragma once",
            "",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            "\n\n".join(blocks),
            "",
        ]
    )
    with open(header, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print(f"{header}: {len(entries)} shader(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
