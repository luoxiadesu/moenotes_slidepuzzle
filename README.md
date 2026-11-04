# moenotes-slidepuzzle

A standalone C17 tool for the in-game sliding puzzle (`SlidePuzzle`) of
`com.bilibili.sirius` 1.1.0. It rebuilds any board offline, reads one out of a
screenshot, and solves it.

> Status: **verified in game** — 3x3 (21 moves) and 4x4 (34 moves) were both solved
> from a device screenshot and cleared by hand. Board generation, screenshot
> recognition, jacket fetching and the CLI are complete; rotation handling and
> stronger solvers for large freshly-shuffled boards are not.

## Why the boards are reproducible

The game builds each puzzle locally:

- `SlidePuzzleBoard.Setup` (`0x6B43268`) sets `_shuffleCount = 11 * N * N`;
- `BuildBoard` (`0x6B43344`) starts from the solved board with the empty slot at
  `(N-1, N-1)`;
- `ShuffleBoard` (`0x6B43820`) drives `System.Random(MasterLiveMusic.MasterId)` and
  applies `11 * N * N` random legal slides.

So a board is a pure function of `(musicId, grid)`, and because every slide is
legal the result is always solvable. Note the solved layout is **not** the identity
permutation: a tile's id counts y from the bottom while cells are listed top-down,
so a cleared 3x3 reads `6 7 8 / 3 4 5 / 0 1 2`. This was confirmed on a device. Full evidence lives in
`work/126_slide-puzzle-analysis/docs/ANALYSIS.md`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires CMake >= 3.16 and a C17 compiler. No third-party runtime dependencies.

## Usage

```bash
./build/moenotes_slidepuzzle gen   100001 4    # board the game will show
./build/moenotes_slidepuzzle solve 100001 4    # shortest-known solution for a fresh board
./build/moenotes_slidepuzzle solve-state 3 '7,2,6,0,3,-1,5,4,1'

# Full pipeline: screenshot + the song's cover art -> board -> moves
./build/moenotes_slidepuzzle read screenshot.png jacket_100001.png 4 --solve

# Same, but fetch/locate the cover art by song id (cached under ~/.cache)
./build/moenotes_slidepuzzle read-music screenshot.png 100001 4 --solve

# Pre-download the whole cover-art library
./build/moenotes_slidepuzzle fetch-jackets --region tw --locale zh-Hant
./build/moenotes_slidepuzzle list-jackets          # 84 built-in song -> jacket rows
```

`read` locates the board in a whole-screen screenshot, slices it N×N, matches each
cell against the N×N slices of the cover art, and solves the assignment so that every
tile is used exactly once. Cells whose match is ambiguous are reported on stderr as
low-confidence rather than silently guessed.

Arrays are in **screen order**: index `i = row * grid + col` with row 0 at the top,
matching what the game draws and what `SlidePuzzleSaveData._tilePositions` records.
`-1` marks the empty slot. A tile's *value* is the position it belongs in, counted
with y from the bottom, which is why the cleared layout is not `0,1,2,...`.

Moves describe the **blank** as seen on screen: `U` moves it up, which is the player
tapping the tile above it.

### Two solvers

Both commands search first and only fall back to replaying the shuffle when the
board is out of reach.

| Grid | Exact A* (weight 1) | Shipped setting | Undo replay |
|---|---|---|---|
| 3x3 | 21 moves, <0.01 s | same | 99 |
| 4x4 | 34 moves, 0.01 s | same | 176 |
| 5x5 | intractable | **71 moves, 0.2 s** | 275 |
| 6x6 | intractable | **132 moves, 1.0 s** | 396 |
| 7x7 | intractable | **250 moves, 2.2 s** (worst 13 s) | 539 |
| 8x8 | intractable | **358 moves, 1.6 s** (worst 7 s) | 704 |

Boards 4x4 and below are solved **optimally**. Above that the search key becomes
`g + weight*h`, which trades optimality for reachability — the reachable state count
is 10^39 at 6x6 and 10^87 at 8x8, so exact search is not an option there.

A single weight does not work on large boards: the bias that makes one board trivial
lets another exhaust its budget, and path length is not monotone in the weight (one
7x7 board needs 289 moves at w=6 but 305 at w=5, found 100x slower). `mnsp_solve_best`
therefore runs a descending ladder of biases and keeps the shortest path any of them
finds. Round 0 is deliberately over-biased — nearly guaranteed to finish — and each
later round is pruned by the best length so far (`g + h >= best` is dropped), so
refinement rounds cost less than the round that produced the bound rather than more.
All 12 boards per size in the benchmark now solve in budget; previously the hardest
7x7 fell back to replaying 539 moves.

`solve <musicId> <grid> --strategy search|undo|auto` picks the path; `auto` (the
default) searches and only replays the shuffle backwards, with a note on stderr, when
the budget is exceeded.

## API

```c
#include "moenotes_slidepuzzle.h"

int32_t positions[16];
mnsp_generate(100001, 4, positions);          /* the board the game shows */

mnsp_move_t moves[1408];
int count = mnsp_solve_by_undo(100001, 4, moves, 1408);
```

All functions borrow their inputs and write into caller buffers; there is no global
state. See `include/moenotes_slidepuzzle.h` for the full contract.

## How `read` works

1. **Board location** — a luma-variance profile over rows and columns finds the single
   contiguous square of busy content; a local sweep then refines the rectangle.
   Tested to ±4 px on 1080×2400 screenshots.
2. **Tile templates** — the cover art is resampled into `grid*grid - 1` mosaics of
   96×96. Index `y*grid + x` follows the game's bottom-up coordinates, and the last
   slice (the blank's home) is never drawn, so it is not a template.
3. **Matching** — zero-mean normalised cross correlation between each cell mosaic and
   every template, then a Hungarian assignment over the (N²−1)×(N²−1) matrix. Solving
   it globally rather than taking per-cell argmax is what fixes the flat-cover cases.
4. **Blank** — the cell with the smallest luma variance is the empty slot; it is
   excluded from the assignment and written as `-1`.

Measured on synthesized 1080×2400 screenshots (JPEG, rescaled, mildly blurred):

| Grid | Cells correct |
|---|---|
| 3×3 … 6×6 | 100 % (every fixture) |

## Cover-art library

`fetch-jackets` downloads each song's cover art into
`$MOENOTES_SLIDEPUZZLE_CACHE` (default `$XDG_CACHE_HOME/moenotes-slidepuzzle`, else
`~/.cache/moenotes-slidepuzzle`) as
`jackets/<region>/<locale>/<musicId>_<assetName>.png`.

The URL is `<base>/<locale>/Image/Jacket/<name>/<name>.png`, where the asset name
(`jkt_<band>_<musicId>`) comes from `MasterLiveMusic._jacketAssetName`. The band
prefix cannot be derived from the song id, so the 84-row mapping is generated into
`src/sp_jackets_builtin.h` by `tools/gen_jacket_table.py <MasterLiveMusic.json>`.

Downloads shell out to `curl` (override with the `fetcher` option) rather than
linking an HTTP client; nothing else in the tool touches the network.

## Verification

- `tests/test_rng.c` replays 8 seeds × 80 draws of real `dotnet 10
  System.Random(seed).Next(0, n)` and requires a bit-exact match, including the
  `INT32_MIN` / `INT32_MAX` wraparound cases.
- `tests/test_puzzle.c` checks 72 generated boards (grid 3..8, 12 music ids) and 30
  undo solutions against the Python reference implementation, plus solvability,
  move application and A* round-trips.
- `tests/test_read.c` runs the whole screenshot pipeline on four synthesized
  screenshots (3×3 … 6×6) and requires an exact permutation match.
- `tests/test_jacket.c` checks the built-in table is a bijection and exercises the
  cache-only resolve path with the network disabled.
  `tools/make_fixture_screens.py <jacket.png> <musicId> <grid> <out.png>` regenerates
  the fixtures; they are synthetic, not captured from the game.

## Known limits

1. Recognition is verified on one device (2410CRP4CC, Android 16, hk-tw-mo, 1.1.0);
   other resolutions, aspect ratios and UI layouts are untested.
2. Flat, mostly-uniform cover art still confuses matching at grid >= 5; those cells
   show up as low-confidence.
3. Rotated or perspective-distorted screenshots are not handled (only straight,
   axis-aligned captures).
4. The built-in jacket table is a snapshot; a new song needs a regenerated table
   (`tools/gen_jacket_table.py`) before `read-music` can find its cover art.
5. Boards above 4x4 are solved approximately: the shipped ladders produce valid paths
   that are typically 2-3.5x the Manhattan lower bound, not provably shortest. Peak
   memory is dominated by the search budget (~210 MiB on the hardest 7x7 board with
   the default 8M-state cap); lower `--max-states` trades length for memory.
6. The tool never reads or writes game saves and never touches the game or its
   servers; it only computes boards and move lists.

## License

The library and CLI are original code. `third_party/stb_image.h` (v2.30, public
domain / MIT) is bundled for the upcoming screenshot stage.
