#!/usr/bin/env python3
"""Re-solve one board in an existing export and replace only that file.

This is the incremental half of the workflow: the full export is generated once, then
individual boards are improved over time (a stronger strategy, a better weight, a
shorter path) without touching the other 503 files.

The replacement is safe to commit because the exporter is deterministic: the same
input always produces the same bytes, so a committed file only changes when the
solution actually changed.

Usage:
  tools/update_board.py <dist-dir> <musicId> <grid> [--strategy S] [--dry-run]

The exporter binary is taken from --binary, then $MOENOTES_SLIDEPUZZLE, then
`moenotes_slidepuzzle` on PATH.
"""
import argparse
import gzip
import json
import os
import shutil
import subprocess
import sys


def read_meta(path):
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as handle:
        return json.loads(handle.readline())


def read_index(directory):
    for name in ("index.jsonl", "index.jsonl.gz"):
        path = os.path.join(directory, name)
        if os.path.exists(path):
            opener = gzip.open if name.endswith(".gz") else open
            with opener(path, "rt", encoding="utf-8") as handle:
                return path, [json.loads(line) for line in handle if line.strip()]
    return None, []


def find_binary(explicit):
    for candidate in (explicit, os.environ.get("MOENOTES_SLIDEPUZZLE"),
                      shutil.which("moenotes_slidepuzzle")):
        if candidate and os.path.exists(candidate):
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory")
    parser.add_argument("music_id", type=int)
    parser.add_argument("grid", type=int)
    parser.add_argument("--strategy", default="auto",
                        help="auto | exact | pdb | ladder (default auto)")
    parser.add_argument("--binary", default=None)
    parser.add_argument("--dry-run", action="store_true",
                        help="solve into a scratch directory and report, change nothing")
    args = parser.parse_args()

    binary = find_binary(args.binary)
    if binary is None:
        print("no exporter binary found; pass --binary or set MOENOTES_SLIDEPUZZLE",
              file=sys.stderr)
        return 2

    index_path, rows = read_index(args.directory)
    if index_path is None:
        print(f"{args.directory} has no index; is it an export directory?", file=sys.stderr)
        return 2
    compressed = index_path.endswith(".gz")

    # The board file's extension has to match the directory: a gzip directory holds
    # .jsonl.gz, a plain one holds .jsonl. Mixing them is what makes an incremental
    # update look corrupt to a downstream reader.
    name = f"{args.music_id}_{args.grid}.jsonl" + (".gz" if compressed else "")
    target = os.path.join(args.directory, name)
    if not os.path.exists(target):
        print(f"{name} is not in {args.directory}", file=sys.stderr)
        return 2

    before = read_meta(target)

    scratch = os.path.join(args.directory, f".update-{args.music_id}-{args.grid}")
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch, exist_ok=True)
    command = [binary, "export", "--out", scratch,
               "--grid", str(args.grid), "--music", str(args.music_id),
               "--deterministic", "--index"]
    if not compressed:
        command.append("--no-compress")
    if args.strategy != "auto":
        command += ["--strategy", args.strategy]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stderr.strip(), file=sys.stderr)
        print("re-solve failed; nothing changed", file=sys.stderr)
        shutil.rmtree(scratch, ignore_errors=True)
        return 1

    new_path = os.path.join(scratch, name)
    if not os.path.exists(new_path):
        print("the exporter did not produce the expected file; nothing changed",
              file=sys.stderr)
        shutil.rmtree(scratch, ignore_errors=True)
        return 1
    after = read_meta(new_path)

    old_len = before["solutionLength"]
    new_len = after["solutionLength"]
    if new_len > old_len:
        print(f"{name}: the new solution is longer ({new_len} vs {old_len}); "
              f"refusing to replace a better file. Use --strategy to pick a "
              f"different search, or delete the file and re-export to force it.",
              file=sys.stderr)
        shutil.rmtree(scratch, ignore_errors=True)
        return 1

    unchanged = old_len == new_len and before.get("solver") == after.get("solver") \
        and before.get("initial") == after.get("initial")
    print(f"{name}: {old_len} steps ({before.get('solver')}) -> "
          f"{new_len} steps ({after.get('solver')})")

    if args.dry_run:
        print("--dry-run: nothing written")
        shutil.rmtree(scratch, ignore_errors=True)
        return 0

    if unchanged:
        print("identical solution; the file is already up to date")
        shutil.rmtree(scratch, ignore_errors=True)
        return 0

    shutil.move(new_path, target)

    # Rewrite the index with the same rows, updating only this board's line. Writing it
    # whole keeps the file a single gzip member, which is what downstream readers need.
    new_rows = []
    replaced = False
    for row in rows:
        if row.get("musicId") == args.music_id and row.get("grid") == args.grid:
            row = dict(row)
            row["solutionLength"] = new_len
            row["solver"] = after.get("solver")
            row["quality"] = after.get("quality")
            replaced = True
        new_rows.append(row)
    if not replaced:
        new_rows.append({
            "musicId": args.music_id,
            "grid": args.grid,
            "file": name,
            "solutionLength": new_len,
            "solver": after.get("solver"),
            "quality": after.get("quality"),
        })

    temporary = index_path + ".tmp"
    opener = gzip.open if compressed else open
    with opener(temporary, "wt", encoding="utf-8") as handle:
        for row in new_rows:
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
    os.replace(temporary, index_path)

    shutil.rmtree(scratch, ignore_errors=True)
    print(f"replaced {name} and updated {os.path.basename(index_path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
