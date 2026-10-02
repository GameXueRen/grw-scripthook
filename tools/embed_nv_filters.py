#!/usr/bin/env python3
"""Embed the compiled night-vision filter shaders into a C header.

Every filter the plugin offers is one compile of plugins/NvFilter/nv_filter_ps.hlsl
with a different pair of defines - which look, and how far it reaches. The four
builds are 3224 to 3948 bytes and differ only in the folded immediate (measured:
one byte, plus the DXBC checksum, plus whatever dead code the constant lets the
compiler drop), so the plugin carries them all and swaps pointers rather than
shipping one plugin per look.

    cd plugins/NvFilter
    fxc /T ps_5_0 /O3 /DNV_DEFAULT_MODE=1 /DNV_ALL_MODES=0 /Fo nv_bw.cso        nv_filter_ps.hlsl
    fxc /T ps_5_0 /O3 /DNV_DEFAULT_MODE=2 /DNV_ALL_MODES=0 /Fo nv_yg.cso        nv_filter_ps.hlsl
    fxc /T ps_5_0 /O3 /DNV_DEFAULT_MODE=1 /DNV_ALL_MODES=1 /Fo nv_bw_all.cso    nv_filter_ps.hlsl
    fxc /T ps_5_0 /O3 /DNV_DEFAULT_MODE=2 /DNV_ALL_MODES=1 /Fo nv_yg_all.cso    nv_filter_ps.hlsl
    python ../../tools/embed_nv_filters.py

Both "look" and "reach" are the ini values and what the plugin matches its two
menu rows on, so a variant may be reordered or added here without touching the
plugin's logic. There is deliberately no build that leaves the look alone: that
is what the plugin's OFF row is, and it reaches it by substituting nothing rather
than by shipping a copy of the engine's own shader.
"""

import hashlib
from pathlib import Path
import zlib

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "plugins" / "NvFilter"
HEADER = PLUGIN / "nv_filters.h"

# (look, reach, compiled file)
VARIANTS = [
    ("black-and-white", "default", "nv_bw.cso"),
    ("yellow-green",    "default", "nv_yg.cso"),
    ("black-and-white", "all",     "nv_bw_all.cso"),
    ("yellow-green",    "all",     "nv_yg_all.cso"),
]

WRAP = 96          # hex characters per line


def blob_of(path):
    data = path.read_bytes()
    if data[:4] != b"DXBC":
        raise SystemExit(f"{path} is not a DXBC blob")
    if len(data) % 4:
        raise SystemExit(f"{path} is not a whole number of dwords")
    return data


def hex_lines(data):
    hexed = data.hex()
    return "\n".join('        "%s"' % hexed[i:i + WRAP]
                     for i in range(0, len(hexed), WRAP))


def main():
    parts, summary = [], []

    for look, reach, name in VARIANTS:
        path = PLUGIN / name
        if not path.exists():
            raise SystemExit(f"missing {path} - compile it first (see the docstring)")
        data = blob_of(path)
        crc = zlib.crc32(data) & 0xFFFFFFFF
        sha = hashlib.sha256(data).hexdigest()
        parts.append(f'''    /* {look} over {reach} - {len(data)} bytes, sha256 {sha} */
    {{
        "{look}",
        "{reach}",
{hex_lines(data)},
        {len(data)}, 0x{crc:08X}u
    }},''')
        summary.append(f" * {look:16} {reach:8} {name:16} {len(data):5} bytes"
                       f"  crc {crc:08X}")

    joined_summary = "\n".join(summary)
    HEADER.write_text(f'''/* Generated from plugins/NvFilter/nv_filter_ps.hlsl - do not edit by hand.
 *
 * Rebuild with:
 *   cd plugins/NvFilter
 *   fxc /T ps_5_0 /O3 /DNV_DEFAULT_MODE=1 /DNV_ALL_MODES=0 /Fo nv_bw.cso        nv_filter_ps.hlsl
 *   fxc /T ps_5_0 /O3 /DNV_DEFAULT_MODE=2 /DNV_ALL_MODES=0 /Fo nv_yg.cso        nv_filter_ps.hlsl
 *   fxc /T ps_5_0 /O3 /DNV_DEFAULT_MODE=1 /DNV_ALL_MODES=1 /Fo nv_bw_all.cso    nv_filter_ps.hlsl
 *   fxc /T ps_5_0 /O3 /DNV_DEFAULT_MODE=2 /DNV_ALL_MODES=1 /Fo nv_yg_all.cso    nv_filter_ps.hlsl
 *   python ../../tools/embed_nv_filters.py
 *
 * Each entry is one compiled replacement for the engine's HDRLighting_NV_ps.
 * `look` and `reach` are the ini values and how the plugin matches its two menu
 * rows to a variant. The "all" builds do not read the selector at all - the
 * compiler folds it to a constant - which is why they are the smaller ones.
 *
{joined_summary}
 */

#ifndef NV_FILTERS_H
#define NV_FILTERS_H

typedef struct NvFilterShader {{
    const char *look;       /* ini value and the plugin's lookup name */
    const char *reach;      /* ditto, for the second row              */
    const char *hex;        /* the DXBC bytes, as hex                 */
    unsigned    bytes;
    unsigned    crc;
}} NvFilterShader;

static const NvFilterShader kNvFilterShaders[] = {{
{chr(10).join(parts)}
}};

#define NV_FILTER_SHADERS \\
    ((int)(sizeof(kNvFilterShaders) / sizeof(kNvFilterShaders[0])))

#endif /* NV_FILTERS_H */
''', encoding="ascii")
    print(f"{HEADER} written:")
    print(joined_summary)


if __name__ == "__main__":
    main()
