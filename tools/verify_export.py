#!/usr/bin/env python3
"""End-to-end verification of an export directory.

The C unit tests cover the writer in isolation; this checks what a consumer would
actually experience: every file parses as strict JSON, every step moves the tile it
claims to move into the blank it claims is blank, every intermediate board matches
the one written, and the last board is the goal.

Usage: verify_export.py <export-dir> [--quiet]
"""
import gzip
import glob
import json
import sys


def check_file(path):
    with gzip.open(path, "rt", encoding="utf-8") as handle:
        lines = [json.loads(line) for line in handle]
    if not lines:
        return ["empty file"]
    meta, steps = lines[0], lines[1:]
    if meta.get("type") != "meta":
        return ["first line is not meta"]

    problems = []
    if len(steps) != meta["solutionLength"]:
        problems.append(
            f"declared {meta['solutionLength']} steps, found {len(steps)}"
        )

    board = [row[:] for row in meta["initial"]]
    for index, step in enumerate(steps, start=1):
        if step.get("index") != index:
            problems.append(f"step {index}: index field is {step.get('index')}")
        from_row, from_col = step["from"]
        to_row, to_col = step["to"]
        tile = board[from_row][from_col]
        if tile != step["tile"]:
            problems.append(f"step {index}: source holds {tile}, declared {step['tile']}")
            break
        if board[to_row][to_col] != -1:
            problems.append(f"step {index}: destination is not blank")
            break
        board[from_row][from_col], board[to_row][to_col] = (
            board[to_row][to_col],
            board[from_row][from_col],
        )
        if board != step["board"]:
            problems.append(f"step {index}: board field does not match the replay")
            break
    else:
        if board != meta["goal"]:
            problems.append("final board is not the goal")
    return problems


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    directory = sys.argv[1]
    quiet = "--quiet" in sys.argv[2:]
    files = sorted(glob.glob(f"{directory}/*.jsonl.gz"))
    if not files:
        print(f"no .jsonl.gz files in {directory}")
        return 1

    failures = 0
    for path in files:
        if path.endswith("index.jsonl.gz"):
            continue
        problems = check_file(path)
        if problems:
            failures += 1
            print(f"{path}:")
            for problem in problems:
                print(f"  {problem}")
        elif not quiet:
            print(f"ok  {path.rsplit('/', 1)[-1]}")
    print(f"\n{len(files)} files checked, {failures} with problems")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
