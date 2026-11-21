#!/usr/bin/env python3
"""Write the two SFO files of a background-app package: param.sfo for the
launcher bubble and param2.sfo for the service.

The key sets, value types and field sizes below are the ones a working
background-app package uses. tailscale-vita's M21 spike found that SFOs made
with vita-mksfoex (more keys, an empty CONTENT_ID) install and register but
the service then fails to start, so the layout is fixed here and only the
identity values are passed in.

CONTENT_ID is HB0000-<TITLE_ID>_00-<TAIL>, with one 16-character TAIL for
both halves. The service's INSTALL_DIR_ADDCONT and INSTALL_DIR_SAVEDATA name
the launcher's TITLE_ID.
"""
import argparse
import struct
from pathlib import Path

TEXT, NUMBER = 0x0204, 0x0404

# (key, type, field size, value). None is filled in from the arguments.
LAUNCHER = [
    ("APP_VER", TEXT, 8, None),
    ("ATTRIBUTE", NUMBER, 4, 0x01009000),
    ("ATTRIBUTE2", NUMBER, 4, 0),
    ("ATTRIBUTE_MINOR", NUMBER, 4, 17),
    ("CATEGORY", TEXT, 4, "gdc"),
    ("CONTENT_ID", TEXT, 48, None),
    ("GC_RO_SIZE", NUMBER, 4, 0),
    ("GC_RW_SIZE", NUMBER, 4, 0),
    ("PARENTAL_LEVEL", NUMBER, 4, 1),
    ("PSP2_DISP_VER", TEXT, 8, "03.600"),
    ("PSP2_SYSTEM_VER", NUMBER, 4, 0x03600000),
    ("REGION_DENY", NUMBER, 4, 0),
    ("SAVEDATA_MAX_SIZE", NUMBER, 4, 1048576),
    ("STITLE", TEXT, 52, None),
    ("TITLE", TEXT, 128, None),
    ("TITLE_ID", TEXT, 12, None),
    ("VERSION", TEXT, 8, "01.00"),
]
SERVICE = [
    ("APP_VER", TEXT, 8, None),
    ("ATTRIBUTE", NUMBER, 4, 0x00081000),
    ("ATTRIBUTE2", NUMBER, 4, 0),
    ("CATEGORY", TEXT, 4, "gdd"),
    ("CONTENT_ID", TEXT, 48, None),
    ("INSTALL_DIR_ADDCONT", TEXT, 12, None),
    ("INSTALL_DIR_SAVEDATA", TEXT, 12, None),
    ("PARENTAL_LEVEL", NUMBER, 4, 1),
    ("REGION_DENY", NUMBER, 4, 0),
    ("SAVEDATA_MAX_SIZE", NUMBER, 4, 1048576),
    ("STITLE", TEXT, 52, None),
    ("TITLE", TEXT, 128, None),
    ("TITLE_ID", TEXT, 12, None),
    ("VERSION", TEXT, 8, "01.00"),
]


def build_sfo(layout, values):
    """The SFO bytes for `layout`, with the None entries taken from `values`."""
    keys, index, data = b"", b"", b""
    for key, kind, size, value in layout:
        if value is None:
            value = values[key]
        raw = struct.pack("<I", value) if kind == NUMBER else value.encode() + b"\0"
        if len(raw) > size:
            raise ValueError(f"{key}: {value!r} does not fit in {size} bytes")
        index += struct.pack("<HHIII", len(keys), kind, len(raw), size, len(data))
        keys += key.encode() + b"\0"
        data += raw.ljust(size, b"\0")
    keys += b"\0" * (-len(keys) % 4)
    key_start = 20 + len(index)
    header = struct.pack("<IIIII", 0x46535000, 0x101, key_start, key_start + len(keys), len(layout))
    return header + index + keys + data


def content_id(title_id, tail):
    return f"HB0000-{title_id}_00-{tail}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, type=Path, help="folder to write param.sfo and param2.sfo into")
    ap.add_argument("--launcher-id", required=True, help="launcher TITLE_ID (9 characters)")
    ap.add_argument("--launcher-title", required=True)
    ap.add_argument("--service-id", required=True, help="service TITLE_ID (9 characters)")
    ap.add_argument("--service-title", required=True)
    ap.add_argument("--tail", required=True, help="16-character CONTENT_ID suffix shared by both halves")
    ap.add_argument("--app-ver", default="01.00")
    args = ap.parse_args()

    if len(args.launcher_id) != 9 or len(args.service_id) != 9:
        ap.error("a TITLE_ID is 9 characters")
    if len(args.tail) != 16:
        ap.error("--tail is 16 characters")

    (args.out / "param.sfo").write_bytes(build_sfo(LAUNCHER, {
        "APP_VER": args.app_ver,
        "CONTENT_ID": content_id(args.launcher_id, args.tail),
        "STITLE": args.launcher_title,
        "TITLE": args.launcher_title,
        "TITLE_ID": args.launcher_id,
    }))
    (args.out / "param2.sfo").write_bytes(build_sfo(SERVICE, {
        "APP_VER": args.app_ver,
        "CONTENT_ID": content_id(args.service_id, args.tail),
        "INSTALL_DIR_ADDCONT": args.launcher_id,
        "INSTALL_DIR_SAVEDATA": args.launcher_id,
        "STITLE": args.service_title,
        "TITLE": args.service_title,
        "TITLE_ID": args.service_id,
    }))
    print(f"wrote {args.out}/param.sfo ({args.launcher_id} \"{args.launcher_title}\") and "
          f"param2.sfo ({args.service_id} \"{args.service_title}\")")


if __name__ == "__main__":
    main()
