#!/usr/bin/env python3
"""Synthesize whole-screen screenshots for the C recognition tests.

Reproduces what the game draws: a phone-sized canvas with a header, a board of
jacket slices (each tile shows the slice of its own *correct* position), the empty
slot rendered as a darker cell, and a footer button row. No game assets are
embedded: the caller supplies a jacket PNG.

Usage:
  python3 tools/make_fixture_screens.py <jacket.png> <musicId> <grid> <out.png>
"""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter


def load_jacket(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float32) / 255.0


def generate_positions(music_id, grid):
    """Same algorithm as the C library (kept independent on purpose)."""
    DIRS = ((0, 1), (0, -1), (-1, 0), (1, 0))
    MBIG, MSEED = 2147483647, 161803398

    def i32(x):
        x &= 0xFFFFFFFF
        return x - 0x100000000 if x >= 0x80000000 else x

    subtraction = 2147483647 if music_id == -2147483648 else abs(music_id)
    mj = i32(MSEED - subtraction)
    state = [0] * 56
    state[55] = mj
    mk = 1
    for i in range(1, 55):
        ii = (21 * i) % 55
        state[ii] = mk
        mk = i32(mj - mk)
        if mk < 0:
            mk = i32(mk + MBIG)
        mj = state[ii]
    for _ in range(1, 5):
        for i in range(1, 56):
            v = i32(state[i] - state[1 + (i + 30) % 55])
            if v < 0:
                v = i32(v + MBIG)
            state[i] = v
    inext, inextp = 0, 21

    def next_int(lo, hi):
        nonlocal inext, inextp
        inext += 1
        if inext >= 56:
            inext = 1
        inextp += 1
        if inextp >= 56:
            inextp = 1
        r = i32(state[inext] - state[inextp])
        if r == MBIG:
            r -= 1
        if r < 0:
            r = i32(r + MBIG)
        state[inext] = r
        return lo + int(r * (1.0 / MBIG) * (hi - lo))

    cells = grid * grid
    # Solved layout in screen order: a tile id counts y from the bottom, so screen
    # row r holds tile row grid-1-r. Matches mnsp_goal_positions and what the game
    # shows on a clear.
    positions = [(grid - 1 - r) * grid + c for r in range(grid) for c in range(grid)]
    positions[cells - 1] = -1
    empty = cells - 1
    for _ in range(11 * grid * grid):
        ex, ey = empty % grid, empty // grid
        cand = []
        for dx, dy in DIRS:
            nx, ny = ex + dx, ey + dy
            if 0 <= nx < grid and 0 <= ny < grid:
                cand.append(ny * grid + nx)
        pick = next_int(0, len(cand))
        positions[empty], positions[cand[pick]] = positions[cand[pick]], positions[empty]
        empty = cand[pick]
    return positions


def compose_screenshot(jacket, positions, grid, width=1080, height=2400,
                       cell=150, gap=8, top=700):
    """Draw a full phone screenshot; returns float RGB in [0,1]."""
    img = np.full((height, width, 3), 0.07, dtype=np.float32)
    img[0:110] = 0.15           # status bar
    img[110:280] = 0.22         # title band

    board_w = grid * cell + (grid - 1) * gap
    margin = (width - board_w) // 2
    img[top - 50:top + board_w + 50, margin - 50:margin + board_w + 50] = 0.02

    for y in range(grid):
        for x in range(grid):
            value = positions[y * grid + x]
            oy = top + (grid - 1 - y) * (cell + gap)
            ox = margin + x * (cell + gap)
            if value < 0:
                img[oy:oy + cell, ox:ox + cell] = 0.10   # empty slot
                continue
            cx, cy = value % grid, value // grid
            h, w = jacket.shape[:2]
            # `value` is a tile id = the position the tile belongs in, with y counted
            # from the bottom. Screen row r therefore shows the tile whose correct
            # row is grid-1-r, i.e. image row = grid-1-cy.
            slice_row = grid - 1 - cy
            crop = jacket[slice_row * h // grid:(slice_row + 1) * h // grid,
                          cx * w // grid:(cx + 1) * w // grid]
            tile = np.asarray(Image.fromarray((crop * 255).astype(np.uint8))
                              .resize((cell, cell), Image.BILINEAR)).astype(np.float32) / 255
            img[oy:oy + cell, ox:ox + cell] = tile

    img[1900:2050, 180:900] = 0.28   # footer buttons
    return img


def degrade(img, target_width=1080, jpeg=78, blur=0.8, gain=1.06, bias=0.02):
    """Phone-screenshot-ish degradation: rescale, mild blur, JPEG, tone shift."""
    import io
    pil = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))
    if target_width != pil.width:
        pil = pil.resize((target_width, int(pil.height * target_width / pil.width)),
                         Image.BILINEAR)
    if blur > 0:
        pil = pil.filter(ImageFilter.GaussianBlur(blur))
    buf = io.BytesIO()
    pil.save(buf, "JPEG", quality=jpeg)
    buf.seek(0)
    arr = np.asarray(Image.open(buf).convert("RGB")).astype(np.float32) / 255
    return np.clip(arr * gain + bias, 0, 1)


def main(argv):
    if len(argv) != 5:
        print(__doc__)
        return 2
    jacket_path, music_id, grid, out = argv[1], int(argv[2]), int(argv[3]), argv[4]
    jacket = load_jacket(jacket_path)
    positions = generate_positions(music_id, grid)
    shot = degrade(compose_screenshot(jacket, positions, grid))
    Image.fromarray((shot * 255).astype(np.uint8)).save(out)
    # The sidecar records the board in the same order the reader emits: screen order,
    # row 0 at the top.
    screen_order = [(grid - 1 - r) * grid + c for r in range(grid) for c in range(grid)]
    recorded = [positions[i] for i in screen_order]
    with open(os.path.splitext(out)[0] + ".txt", "w") as f:
        f.write("%d %d\n" % (music_id, grid))
        f.write(",".join(str(v) for v in recorded) + "\n")
    print("wrote", out, "musicId", music_id, "grid", grid)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
