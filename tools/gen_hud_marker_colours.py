"""Read the faction colours of the 3D markers out of the game's own archive.

Every 3D marker is a Phoenix widget tree, and the colour does not live on the
widget: it lives in a "style" resource named after the colour -
HUD_Marker_Red, HUD_Marker_Purple, HUD_Marker_Green, HUD_Marker_Blue,
HUD_Marker_White, HUD_Marker_Gold - and each marker variant depends on exactly
one of them.

    HUD_Marker_Hostile_SantaBlanca_6D225ADC-...  ->  HUD_Marker_Red
    HUD_Marker_Hostile_Unidad_DF04C8B4-...      ->  HUD_Marker_Purple
    HUD_Marker_FriendlyNPC_43B96827-...         ->  HUD_Marker_Green
    HUD_Marker_FriendlyPlayer_852A9DC7-...      ->  HUD_Marker_Blue
    HUD_Marker_Hostile_White_D26113FF-...       ->  HUD_Marker_White

So the colour a marker draws with is the RGB stored in its style resource, and
that is what this script pulls out:

    python tools/gen_hud_marker_colours.py            # write the header
    python tools/gen_hud_marker_colours.py --list     # just print the table

The style resource is a PHXTS blob.  The colour is four floats - R, G, B in
0..255 (not 0..1) and a fourth value that is 255 on five styles and 1.0 on
Green - followed by a zero float and then the resource's own GUID as text.
The scan below therefore looks for exactly that shape and refuses to guess: a
style with no match, or with more than one, is an error rather than a blank.

The archive must be readable but the game does not have to be closed: this
script only reads.
"""

import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_GAME = r"F:/UbisoftGames/Tom Clancy's Ghost Recon Wildlands"
OUT = os.path.join(ROOT, "plugins", "MoreHudControl", "hud_marker_colours.h")

ARCHIVES = ("DataPC_extra.forge", "DataPC_extra_patch_01.forge")

# name -> (guid, who wears it)
STYLES = (
    ("Red", "ECDB3EE7-0469-41D7-BD2E-A590EF2EDDA8",
     "Santa Blanca, generic hostile, PvP player, DLC PMC"),
    ("Purple", "E0E7995A-1506-43E3-9BC9-6B3E49F1A852",
     "Unidad"),
    ("Green", "7BD74054-A06C-4480-AD00-62935C048825",
     "friendly NPC, i.e. the rebels"),
    ("Blue", "E3579592-03BC-4D7B-B043-4933720977C9",
     "friendly player, i.e. the squad"),
    ("White", "EE725D8E-59FE-47D6-AEE3-67AB74EFA2BC",
     "HVT and the neutral marker"),
    ("Gold", "DDE5A3DB-6BB7-4A15-B942-EA460DE5B705",
     "not worn by any faction marker in this build"),
)

MAGIC = b"\x83\x89\x8d\xdb"


def _forge_io_dir():
    """third_party sits beside the repository, not inside it."""
    for base in (ROOT, os.path.dirname(ROOT)):
        path = os.path.join(base, "third_party", "wildlands-hud")
        if os.path.exists(os.path.join(path, "forge_io.py")):
            return path
    raise SystemExit("forge_io.py not found; expected third_party/wildlands-hud "
                     "beside the repository")


def load_archive(path):
    """(entries, reader) for a scimitar forge, without depending on the toolkit."""
    sys.path.insert(0, _forge_io_dir())
    from forge_io import ForgeArchive, LzoCodec, decode_payload
    archive = ForgeArchive(path)
    codec = LzoCodec()

    def body_of(name):
        hits = archive.find(name)
        if len(hits) != 1:
            return None
        return decode_payload(archive.read_raw(hits[0]), codec).sets[1].data

    return archive, body_of


def colour_of(body):
    """The (r, g, b, a) floats of one PHXTS style resource.

    Anchored on the record header, so the offsets do not depend on the length
    of the resource name: [class][body_size][name_len][name]\\0[body...].
    """
    _cls, size, name_len = struct.unpack_from("<III", body, 0)
    start = 12 + name_len + 1
    end = start + size
    if body[:4] != MAGIC or end > len(body):
        raise ValueError("not a PHXTS resource body")
    found = []
    for at in range(start, end - 20):
        v = struct.unpack_from("<5f", body, at)
        if not all(0.0 <= x <= 255.5 for x in v[:4]):
            continue
        if v[3] not in (255.0, 1.0):
            continue
        if v[0] == 255.0 and v[1] == 255.0 and v[2] == 255.0:
            continue                       # the plain white base block
        if v[4] != 0.0:
            continue
        found.append((at, v[:4]))
    if len(found) != 1:
        raise ValueError("expected exactly one colour block, found %d" % len(found))
    at, (r, g, b, a) = found[0]
    return at - start, tuple(int(round(x)) for x in (r, g, b)), a


def collect(game):
    rows = []
    for name, guid, note in STYLES:
        full = "HUD_Marker_%s_%s" % (name, guid)
        for arc in ARCHIVES:
            path = os.path.join(game, arc)
            if not os.path.exists(path):
                continue
            _, body_of = load_archive(path)
            body = body_of(full)
            if body is None:
                continue                   # this archive does not override it
            off, rgb, alpha = colour_of(body)
            rows.append((name, rgb, alpha, off, arc, note))
            break
        else:
            rows.append((name, None, None, None, None, note))
    return rows


def emit(rows, out_path):
    lines = [
        "/* Generated by tools/gen_hud_marker_colours.py - do not edit by hand.",
        " *",
        " * The colour each 3D marker draws with, read from the style resource its",
        " * widget tree depends on (HUD_Marker_<Colour> in DataPC_extra.forge).",
        " * Values are the ones stored in the archive: RGB in 0..255, not 0..1.",
        " */",
        "",
        "#ifndef HUD_MARKER_COLOURS_H",
        "#define HUD_MARKER_COLOURS_H",
        "",
        "typedef struct {",
        "    const char *style;        /* HUD_Marker_<style> suffix */",
        "    unsigned char r, g, b;    /* as stored: 0..255 */",
        "    const char *worn_by;",
        "} HudMarkerColour;",
        "",
        "static const HudMarkerColour kMarkerColours[] = {",
    ]
    for name, rgb, alpha, off, arc, note in rows:
        if rgb is None:
            lines.append('    { "%s", 0, 0, 0, "%s (UNREAD)" },' % (name, note))
        else:
            lines.append('    { "%s", %3d, %3d, %3d, "%s" },' % (name, rgb[0], rgb[1], rgb[2], note))
    lines += [
        "};",
        "",
        "#define MARKER_COLOUR_COUNT ((int)(sizeof(kMarkerColours) / sizeof(kMarkerColours[0])))",
        "",
        "#endif",
        "",
    ]
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game-dir", default=DEFAULT_GAME)
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--list", action="store_true", help="print the table only")
    args = ap.parse_args()

    rows = collect(args.game_dir)
    missing = [r[0] for r in rows if r[1] is None]
    print("%-8s %-18s %-6s %-8s %s" % ("style", "RGB", "alpha", "body+", "worn by"))
    for name, rgb, alpha, off, arc, note in rows:
        if rgb is None:
            print("%-8s %-18s %-6s %-8s %s" % (name, "-", "-", "-", note))
        else:
            print("%-8s #%02X%02X%02X (%3d,%3d,%3d) %5.1f  +0x%03X  %s"
                  % (name, rgb[0], rgb[1], rgb[2], rgb[0], rgb[1], rgb[2],
                     alpha, off, note))
    if missing:
        print("\nnot read: %s - the archive may have changed" % ", ".join(missing))
    if args.list:
        return 1 if missing else 0
    if missing:
        print("refusing to write a header with blanks", file=sys.stderr)
        return 1
    emit(rows, args.out)
    print("\nwrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
