#!/usr/bin/env python3
"""Regenerate src/sp_jackets_builtin.h from a MasterLiveMusic.json snapshot.

The jacket asset name embeds the band ("jkt_<band>_<musicId>"), and the band cannot
be derived from the song id, so the mapping has to be carried. It is small: 84 rows
for the current master.

Usage: python3 tools/gen_jacket_table.py <MasterLiveMusic.json> [out.h]
"""
import json
import sys

TEMPLATE_HEAD = """/* Generated from MasterLiveMusic (%s). Do not edit by hand.
 * Regenerate with tools/gen_jacket_table.py after a master refresh. */
#ifndef SP_JACKETS_BUILTIN_H
#define SP_JACKETS_BUILTIN_H

typedef struct sp_jacket_entry {
    int32_t music_id;
    const char *asset_name;
} sp_jacket_entry_t;

static const sp_jacket_entry_t SP_BUILTIN_JACKETS[] = {
"""
TEMPLATE_TAIL = """};

#define SP_BUILTIN_JACKET_COUNT %d

#endif /* SP_JACKETS_BUILTIN_H */
"""


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    source = argv[1]
    out = argv[2] if len(argv) > 2 else "src/sp_jackets_builtin.h"
    with open(source) as f:
        data = json.load(f)
    rows = data["_allData"] if isinstance(data, dict) else data
    pairs = sorted((r["_id"], r["_jacketAssetName"]) for r in rows)
    with open(out, "w") as f:
        f.write(TEMPLATE_HEAD % "MasterLiveMusic.json")
        for music_id, name in pairs:
            f.write('    { %d, "%s" },\n' % (music_id, name))
        f.write(TEMPLATE_TAIL % len(pairs))
    print("wrote %d entries to %s" % (len(pairs), out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
