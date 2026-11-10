#!/usr/bin/env python3
"""End-to-end verification of an export directory.

The C unit tests cover the writer in isolation; this checks what a consumer would
actually experience: every file parses as strict JSON, every step moves the tile it
claims to move into the blank it claims is blank, every intermediate board matches
the one written, and the last board is the goal.

The `move` letter is checked against `from`/`to` as well. A consumer that walks the
tile endpoints will never notice a wrong letter, so a generation bug that flips only
the vertical letters passes every other check here -- which is exactly what happened,
and why this one is spelled out rather than left implied by the endpoints.

Both the gzip'd (.jsonl.gz) and plain (.jsonl) forms are accepted, and a directory
holding both is fine -- an incremental update can legitimately leave a mix.

The index is checked too when one is present: every board file must be listed, every
listed file must exist, and the declared solutionLength must match what the file
actually contains. That is what makes `dist/` safe to update one board at a time.

Usage: verify_export.py <export-dir> [--quiet] [--no-index-check]
"""
import gzip
import glob
import json
import os
import sys


def open_text(path):
    """Open a board file as text, transparently handling both forms.

    The suffix is only a hint. A file named .jsonl that actually holds gzip bytes (a
    leftover from switching --no-compress, or a copy that kept the wrong name) is
    detected by its magic number and reported as a naming mismatch rather than as a
    decode error, which is what a downstream reader would otherwise have to puzzle out.
    """
    with open(path, "rb") as probe:
        magic = probe.read(2)
    is_gzip = magic == b"\x1f\x8b"
    if is_gzip and not path.endswith(".gz"):
        raise ValueError(
            "file is gzip-compressed but is not named .gz "
            "(left over from a different --no-compress setting?)"
        )
    if path.endswith(".gz") and not is_gzip:
        raise ValueError("file is named .gz but is not gzip-compressed")
    if is_gzip:
        return gzip.open(path, "rt", encoding="utf-8")
    return open(path, "rt", encoding="utf-8")


def expected_move(from_row, from_col, to_row, to_col):
    """The `move` field names the blank's travel, which is the reverse of the tile's.

    The tile goes from `from` to `to`; the blank goes from `to` to `from`. So a tile
    that ends up in a higher row (on screen) means the blank moved down ("D").
    """
    if to_row < from_row:
        return "D"  # tile moved up the screen, blank moved down
    if to_row > from_row:
        return "U"
    if to_col < from_col:
        return "R"  # tile moved left, blank moved right
    return "L"


def check_file(path):
    try:
        with open_text(path) as handle:
            lines = [json.loads(line) for line in handle]
    except (OSError, ValueError, EOFError) as error:
        return [f"cannot read: {error}"]
    if not lines:
        return ["empty file"]
    meta, steps = lines[0], lines[1:]
    if meta.get("type") != "meta":
        return ["first line is not meta"]
    for field in ("solutionLength", "initial", "goal", "musicId", "grid"):
        if field not in meta:
            return [f"meta is missing {field}"]

    problems = []
    if len(steps) != meta["solutionLength"]:
        problems.append(
            f"declared {meta['solutionLength']} steps, found {len(steps)}"
        )
    if len(meta["initial"]) != meta["grid"]:
        problems.append("initial board does not match the declared grid")

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
        want = expected_move(from_row, from_col, to_row, to_col)
        if step.get("move") != want:
            problems.append(
                f"step {index}: move is {step.get('move')!r}, "
                f"but from/to imply {want!r}"
            )
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


def read_index(directory):
    """Returns (rows, path) or (None, None) when the directory has no index."""
    for name in ("index.jsonl", "index.jsonl.gz"):
        path = os.path.join(directory, name)
        if os.path.exists(path):
            with open_text(path) as handle:
                return [json.loads(line) for line in handle if line.strip()], path
    return None, None


def check_index(directory, rows, boards):
    """Cross-checks the index against what is on disk.

    boards maps a file name to the solutionLength its meta line declares.
    """
    problems = []
    listed = {}
    for row in rows:
        name = row.get("file")
        if not name:
            problems.append(f"index row without a file field: {row}")
            continue
        if name in listed:
            problems.append(f"index lists {name} more than once")
        listed[name] = row

    for name, row in listed.items():
        if name not in boards:
            problems.append(f"index lists {name}, which is not on disk")
            continue
        declared = row.get("solutionLength")
        actual = boards[name]
        if declared != actual:
            problems.append(
                f"{name}: index says {declared} steps, file has {actual}"
            )
    for name in boards:
        if name not in listed:
            problems.append(f"{name} is on disk but missing from the index")
    return problems


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    directory = sys.argv[1]
    quiet = "--quiet" in sys.argv[2:]
    skip_index = "--no-index-check" in sys.argv[2:]

    files = sorted(
        glob.glob(f"{directory}/*.jsonl.gz") + glob.glob(f"{directory}/*.jsonl")
    )
    boards = [f for f in files if not os.path.basename(f).startswith("index.jsonl")]
    if not boards:
        print(f"no board files in {directory}")
        return 1

    failures = 0
    lengths = {}
    for path in boards:
        name = os.path.basename(path)
        problems = check_file(path)
        if problems:
            failures += 1
            print(f"{path}:")
            for problem in problems:
                print(f"  {problem}")
        else:
            with open_text(path) as handle:
                lengths[name] = json.loads(handle.readline())["solutionLength"]
            if not quiet:
                print(f"ok  {name}")

    index_problems = []
    if not skip_index:
        rows, index_path = read_index(directory)
        if rows is None:
            print("\nnote: no index in this directory; skipping the index check")
        else:
            index_problems = check_index(directory, rows, lengths)
            if index_problems:
                failures += 1
                print(f"\n{index_path}:")
                for problem in index_problems:
                    print(f"  {problem}")

    print(f"\n{len(files)} files checked, {failures} with problems")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
