#!/usr/bin/env python3
"""Contract test for tools/update_board.py.

The updater rewrites one board file in an existing export and touches nothing else.
That property is what makes committing dist/ and improving boards in place safe, so
it is checked directly: the other files must keep their exact bytes, the index must
change on exactly one row, and a longer solution must be refused.

Invoked by CTest as:
  test_update_board.py <python> <update_board.py> <verify_export.py> <exporter> <workdir>
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def digest(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def snapshot(directory):
    """Maps file name -> sha256 for every board file, skipping the index."""
    result = {}
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".jsonl") and not name.endswith(".jsonl.gz"):
            continue
        if name.startswith("index.jsonl"):
            continue
        result[name] = digest(os.path.join(directory, name))
    return result


def read_index(directory):
    for name in ("index.jsonl", "index.jsonl.gz"):
        path = os.path.join(directory, name)
        if os.path.exists(path):
            with open(path, "rt", encoding="utf-8") as handle:
                return [json.loads(line) for line in handle if line.strip()]
    return None


def main():
    if len(sys.argv) != 6:
        print("usage: test_update_board.py <python> <updater> <verifier> "
              "<exporter> <workdir>", file=sys.stderr)
        return 2
    python, updater, verifier, exporter, workdir = sys.argv[1:6]

    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    dist = os.path.join(workdir, "dist")

    # A plain-text directory, matching how the repository publishes dist/.
    made = run([exporter, "export", "--out", dist, "--grid", "5",
                "--music", "100001", "--music", "100050", "--music", "100003",
                "--strategy", "ladder", "--no-compress", "--deterministic", "--index"])
    if made.returncode != 0:
        print(f"setup export failed: {made.stderr}", file=sys.stderr)
        return 1

    before = snapshot(dist)
    index_before = read_index(dist)
    if len(before) != 3 or index_before is None or len(index_before) != 3:
        print(f"setup produced {len(before)} boards and "
              f"{len(index_before) if index_before else 0} index rows", file=sys.stderr)
        return 1
    old_length = next(r["solutionLength"] for r in index_before
                      if r["musicId"] == 100050 and r["grid"] == 5)

    # 1. Improve one board. The PDB path is shorter than the ladder on these boards.
    improved = run([python, updater, dist, "100050", "5",
                    "--strategy", "pdb", "--binary", exporter])
    if improved.returncode != 0:
        print(f"update failed: {improved.stderr}{improved.stdout}", file=sys.stderr)
        return 1

    after = snapshot(dist)
    changed = [name for name in before if before[name] != after.get(name)]
    if changed != ["100050_5.jsonl"]:
        print(f"expected only 100050_5.jsonl to change, saw {changed}", file=sys.stderr)
        return 1

    index_after = read_index(dist)
    if index_after is None:
        print("the index disappeared", file=sys.stderr)
        return 1
    new_length = next(r["solutionLength"] for r in index_after
                      if r["musicId"] == 100050 and r["grid"] == 5)
    if new_length >= old_length:
        print(f"the update did not shorten the solution "
              f"({old_length} -> {new_length})", file=sys.stderr)
        return 1
    untouched = [r for r in index_after if not (r["musicId"] == 100050 and r["grid"] == 5)]
    untouched_before = [r for r in index_before
                        if not (r["musicId"] == 100050 and r["grid"] == 5)]
    if untouched != untouched_before:
        print(f"the index changed rows it should not have: "
              f"{untouched_before} -> {untouched}", file=sys.stderr)
        return 1

    # 2. The result must still verify end to end, index cross-check included.
    verified = run([python, verifier, dist, "--quiet"])
    if verified.returncode != 0:
        print(f"verify failed: {verified.stdout}{verified.stderr}", file=sys.stderr)
        return 1

    # 3. Re-running is idempotent: same board, same bytes.
    snapshot_after_first = snapshot(dist)
    again = run([python, updater, dist, "100050", "5",
                 "--strategy", "pdb", "--binary", exporter])
    if again.returncode != 0:
        print(f"re-run failed: {again.stderr}{again.stdout}", file=sys.stderr)
        return 1
    if snapshot(dist) != snapshot_after_first:
        print("re-running the updater changed files", file=sys.stderr)
        return 1

    # 4. A longer solution must be refused, and the directory left as it was.
    before_refusal = snapshot(dist)
    index_before_refusal = read_index(dist)
    refused = run([python, updater, dist, "100050", "5",
                   "--strategy", "ladder", "--binary", exporter])
    if refused.returncode == 0:
        print("the updater replaced a better solution with a worse one", file=sys.stderr)
        return 1
    if snapshot(dist) != before_refusal or read_index(dist) != index_before_refusal:
        print("a refused update still modified the directory", file=sys.stderr)
        return 1

    print("  update_board: isolates the change, keeps the index honest, "
          "is idempotent, refuses regressions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
