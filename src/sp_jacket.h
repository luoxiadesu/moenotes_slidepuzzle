/* Jacket library: resolve a music id to cover art, cache the PNGs on disk.
 * Internal to the library. */
#ifndef SP_JACKET_H
#define SP_JACKET_H

#include <stdint.h>

#include "moenotes_slidepuzzle.h"

/* Look up the jacket asset name (e.g. "jkt_001_100001") for a song id.
 * Returns NULL when the id is not in the built-in table. */
const char *sp_jacket_asset_name(int32_t music_id);

/* Number of entries in the built-in table. */
int sp_jacket_builtin_count(void);

/* Built-in entry accessor, for enumerating the whole library. Returns 0 when the
 * index is out of range. */
int sp_jacket_builtin_at(int index, int32_t *out_music_id, const char **out_asset_name);

/* Where cached files live, derived from the environment:
 *   $MOENOTES_SLIDEPUZZLE_CACHE, else $XDG_CACHE_HOME/moenotes-slidepuzzle,
 *   else $HOME/.cache/moenotes-slidepuzzle, else ./.cache/moenotes-slidepuzzle.
 * Writes the directory into out[0..capacity). Returns 0 on overflow. */
int sp_cache_dir(char *out, size_t capacity);

/* Absolute path of the cached jacket PNG for a song. */
int sp_jacket_cache_path(char *out, size_t capacity, const char *region,
                         const char *locale, int32_t music_id);

/* Download http(s)://... into `destination` by running `fetcher destination url`.
 * `fetcher` is argv[0] of the helper, e.g. "curl"; "curl" gets "-fsSL -o" prepended
 * automatically, any other program is invoked as `<fetcher> <url> <destination>`.
 * Returns MNSP_OK when the helper exits 0 and the file is non-empty. */
mnsp_result_t sp_fetch_to_file(const char *fetcher, const char *url, const char *destination);

/* Build the jacket URL for a region/locale. Uses the public assets mirror layout:
 *   https://assets.bdon.moe/<locale>/Image/Jacket/<name>/<name>.png */
int sp_jacket_url(char *out, size_t capacity, const char *base_url, const char *locale,
                  const char *asset_name);

#endif /* SP_JACKET_H */
