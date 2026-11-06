/* Disjoint pattern databases for the 5x5 sliding puzzle. Internal to the library.
 *
 * The tables are owned by an mnsp_pdb_t: there is no global state, so independent
 * instances can coexist (required by the "no global mutable state" contract in
 * moenotes_slidepuzzle.h, and by a WASM build that may host more than one solver).
 *
 * Only the 6-tile split is implemented. Building it costs about 3 minutes and 530 MiB
 * of transient memory; the finished tables occupy 486 MiB. Callers that cannot afford
 * that should skip the PDB entirely -- mnsp_solve_best does not require it, and its
 * max_states parameter is the cheaper lever on memory (see docs/PDB-INTEGRATION.md).
 *
 * A 5-tile split (25P5, 24 MiB total) is a plausible smaller variant and measured
 * competitively on path length in early sampling, but it is not implemented here and
 * the comparison was not completed, so nothing in this header should be read as
 * promising it.
 */
#ifndef SP_PDB_H
#define SP_PDB_H

#include "moenotes_slidepuzzle.h"

typedef struct mnsp_pdb mnsp_pdb_t;

/* 1 when this board size has a pattern database (5 only). */
int mnsp_pdb_supported(int grid);

/* Build the tables. Returns NULL on an unsupported size or allocation failure. */
mnsp_pdb_t *mnsp_pdb_create(int grid);

/* Release the tables and the handle. Safe on NULL. */
void mnsp_pdb_destroy(mnsp_pdb_t *pdb);

/* 1 when the handle holds usable tables. */
int mnsp_pdb_ready(const mnsp_pdb_t *pdb);

/* Admissible heuristic: the sum of the four disjoint pattern databases.
 * Returns -1 when the handle is not ready, the grid is not 5, or the board is not a
 * valid 5x5 position. The result never exceeds the true optimal distance. */
int mnsp_pdb_heuristic(const mnsp_pdb_t *pdb, const int32_t *positions, int grid);

#endif /* SP_PDB_H */
