#!/usr/bin/env python3
"""Compile the night-vision look shaders and embed them into a C header.

Every look the plugin offers is one compile of plugins/NvFilter/nv_filter_ps.hlsl
with a different pair of defines - which look, and how far it reaches - and the
plugin carries all of them, swapping pointers, rather than shipping one plugin
per look. Seventeen looks over two reaches is thirty-four builds of 3.2 to 4.7
KB, embedding to about 320 KB of hex.

    python tools/embed_nv_filters.py --build    # compile, then embed
    python tools/embed_nv_filters.py            # embed what is already there

This tool owns both steps on purpose: VARIANTS below is the one list of looks, so
what gets compiled and what gets embedded cannot drift apart. Writing the list
out again in a shell script is how that goes wrong.

Both "look" and "reach" are the ini values and what the plugin matches its two
menu rows on, so a variant may be reordered or added here without touching the
plugin's logic. There is deliberately no build that leaves the look alone: that
is what the plugin's OFF row is, and it reaches it by substituting nothing rather
than by shipping a copy of the engine's own shader.
"""

import hashlib
import subprocess
import sys
from pathlib import Path
import zlib

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "plugins" / "NvFilter"
HEADER = PLUGIN / "nv_filters.h"
HLSL = "nv_filter_ps.hlsl"

# The Windows SDK's shader compiler, hardcoded like every other toolchain path in
# this repo, and -O3 to match what the checked-in .cso were built with so that a
# rebuild reproduces them.
FXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"

# (look, reach, compiled file). Order is the mode order: the look a row first
# appears for is mode 1, the next mode 2, and so on - the numbering nv_looks.hlsl
# declares, which check_looks.py verifies against this list. A look also needs a
# row in gen_looks.py's LOOKS, in the plugin's kLooks and in its lang.ini.
VARIANTS = [
    ("black-and-white",  "default", "nv_bw.cso"),
    ("black-and-white",  "all",     "nv_bw_all.cso"),
    ("yellow-green",     "default", "nv_yg.cso"),
    ("yellow-green",     "all",     "nv_yg_all.cso"),
    ("sepia",            "default", "nv_sepia.cso"),
    ("sepia",            "all",     "nv_sepia_all.cso"),
    ("apollo",           "default", "nv_apollo.cso"),
    ("apollo",           "all",     "nv_apollo_all.cso"),
    ("lighthouse",       "default", "nv_lighthouse.cso"),
    ("lighthouse",       "all",     "nv_lighthouse_all.cso"),
    ("tennessee",        "default", "nv_tennessee.cso"),
    ("tennessee",        "all",     "nv_tennessee_all.cso"),
    ("arlington",        "default", "nv_arlington.cso"),
    ("arlington",        "all",     "nv_arlington_all.cso"),
    ("bridge",           "default", "nv_bridge.cso"),
    ("bridge",           "all",     "nv_bridge_all.cso"),
    ("montenegro",       "default", "nv_montenegro.cso"),
    ("montenegro",       "all",     "nv_montenegro_all.cso"),
    ("chief",            "default", "nv_chief.cso"),
    ("chief",            "all",     "nv_chief_all.cso"),
    ("songbird",         "default", "nv_songbird.cso"),
    ("songbird",         "all",     "nv_songbird_all.cso"),
    ("madre",            "default", "nv_madre.cso"),
    ("madre",            "all",     "nv_madre_all.cso"),
    ("burner",           "default", "nv_burner.cso"),
    ("burner",           "all",     "nv_burner_all.cso"),
    ("neutral",          "default", "nv_neutral.cso"),
    ("neutral",          "all",     "nv_neutral_all.cso"),
    ("amber",            "default", "nv_amber.cso"),
    ("amber",            "all",     "nv_amber_all.cso"),
    ("cool-blue",        "default", "nv_coolblue.cso"),
    ("cool-blue",        "all",     "nv_coolblue_all.cso"),
    ("contrast",         "default", "nv_contrast.cso"),
    ("contrast",         "all",     "nv_contrast_all.cso"),
]

WRAP = 96          # hex characters per line


def looks_in_order():
    """The looks, in the order VARIANTS first mentions them."""
    seen = []
    for look, _, _ in VARIANTS:
        if look not in seen:
            seen.append(look)
    return seen


def mode_of(look):
    """A look's NV_DEFAULT_MODE: its 1-based position. nv_looks.hlsl declares the
    same numbering and check_looks.py holds the two lists together."""
    return looks_in_order().index(look) + 1


def build():
    """Compile every variant.

    fxc writes where it is told or not at all, and a missing .cso would otherwise
    only be noticed by -dumpbin much later, so a failure stops the run here.
    """
    if not Path(FXC).exists():
        raise SystemExit(f"fxc not found at {FXC}\n"
                         "Install the Windows SDK, or edit FXC at the top of "
                         "this file.")
    for look, reach, cso in VARIANTS:
        args = [FXC, "-T", "ps_5_0", "-O3",
                f"-DNV_DEFAULT_MODE={mode_of(look)}",
                f"-DNV_ALL_MODES={1 if reach == 'all' else 0}",
                f"-Fo{cso}", HLSL]
        r = subprocess.run(args, cwd=PLUGIN, capture_output=True, text=True)
        if r.returncode or not (PLUGIN / cso).exists():
            raise SystemExit(f"fxc failed for {look}/{reach}:\n"
                             f"{r.stdout}{r.stderr}")
    print(f"{len(VARIANTS)} shaders compiled into {PLUGIN}")


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
            raise SystemExit(f"missing {path} - run this file with --build")
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
 *   python tools/embed_nv_filters.py --build
 *
 * nv_filter_ps.hlsl includes nv_looks.hlsl, which is generated separately from
 * the extracted curves (see .codebuddy/plans/nightvision-more-looks_3f7c21d0.md
 * and check_looks.py), so that file has to be current before the fxc runs.
 *
 * Each entry is one compiled replacement for the engine's HDRLighting_NV_ps.
 * `look` and `reach` are the ini values and how the plugin matches its two menu
 * rows to a variant. The "all" builds do not read the selector at all - the
 * compiler folds it to a constant - which is why they are the smaller ones.
 *
 * Which look a build substitutes is a preprocessor choice (LookSelected in
 * nv_looks.hlsl), not a run-time one, so a variant carries the one look it was
 * built for. Every single-table variant therefore weighs exactly the same.
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
    if "--build" in sys.argv[1:]:
        build()
    main()
