#ifndef STOMA_H
#define STOMA_H

#include <stddef.h>
#include <stdint.h>
#include <ttypt/rec.h>

/*
 * stoma — qmap-backed full-text index.
 *
 * A generic inverted index over (field, token) → row_id. It knows nothing
 * about rows, schemas, or the application: callers choose which fields to
 * index and interpret row_ids. Match semantics are word-token based with
 * prefix expansion: a query token must match the start of an indexed token.
 * Multiple tokens in a query are ANDed.
 */

/*
 * Fold a UTF-8 string to lowercase, preserving accents (accent-sensitive
 * search). ASCII A-Z and the Latin-1 Supplement uppercase letters are
 * lowercased; all other bytes are copied verbatim. The fold is
 * locale-independent (no iconv, no setlocale) and never grows the output,
 * so it fits whenever outsz > strlen(in). Returns the number of bytes
 * written, or -1 if the output buffer is too small (caller falls back to
 * raw comparison).
 */
int stoma_fold(char *out, size_t outsz, const char *in);

typedef struct stoma_db stoma_db_t;

/* Open an index. mask is the qmap hash mask (0 = qmap default). */
stoma_db_t *stoma_open(unsigned mask);

/*
 * rec_axis_open (RECALL-KERNEL.md "rec_axis_open convention", optional CLI-open convention,
 * not part of libqmap's core rec_query registry API): opens a stoma
 * index from an opaque spec string and returns the ctx a caller then
 * passes to rec_axis_set_ctx() -- the stoma_db_t* directly.
 *
 * Two spec shapes (2B-2):
 *  - decimal mask (or empty/NULL): the stoma_open() mask, 0 = qmap
 *    default — a plain in-memory index, nothing seeded.
 *  - a path (contains '/'): the 2B-2 primary-seeded rebuild. The
 *    directory is scanned for a `<primary>.roster` sidecar (written by
 *    the qmap CLI's roster persistence); the primary qmap store it
 *    prefixes is opened (`:a:s` raw and `:a:u` decimal alike) and every
 *    record is re-indexed under the canonical `text` field. A rebuild
 *    emits a one-line budget note (docs + ms) on stderr so callers can
 *    measure (mm-plan U4). A path whose sidecar is missing or whose
 *    primary cannot be opened warns loudly and falls back to an empty
 *    memory index — it never silently serves stale or empty results.
 */
void *rec_axis_open(const char *spec);

/* Close the index and free all resources. */
void stoma_close(stoma_db_t *db);

/* Drop all entries; the handle stays valid. */
void stoma_clear(stoma_db_t *db);

/*
 * Index every token of value under (field, row_id).
 * Duplicate tokens collapse automatically. Returns 0 on success,
 * -1 on invalid arguments.
 */
int stoma_index(stoma_db_t *db,
	const char *field, const char *row_id, const char *value);

/*
 * rec_ref_t-native convenience wrapper for callers whose ids are already
 * a rec_ref_t (the id-uniformity boundary shared with libjoint/libsepal/
 * libislet's rec_axis_fill_* functions): formats row_id as the canonical
 * decimal string stoma_rank/rec_axis_fill_tokens expect and calls
 * stoma_index. This does not change stoma's storage or key format, and
 * does not remove the generic string-row_id path (stoma_index proper) --
 * it only saves every rec_ref_t-keyed caller from doing its own
 * decimal formatting before indexing. Same return contract as
 * stoma_index.
 */
int stoma_index_ref(stoma_db_t *db,
	const char *field, rec_ref_t row_id, const char *value);

/*
 * Remove every posting the axis's index created for (field, row_id),
 * plus the doc side-table entry — the differential inverse of
 * stoma_index (Phase 2A `rec_axis_unstore` mechanism; RECALL-KERNEL.md
 * "rec_axis_store convention"): index → unindex leaves zero residual
 * postings for the row and no doc entry, at a cost ∝ tokens of the row
 * (never O(store), never a scan — the doc side-table is walked
 * backwards to recover the exact posting keys). Idempotent: an absent
 * (field, row) reads as 0, not an error. Returns 0 on success, -1 on
 * invalid arguments. Zero new stored state; memory-only.
 */
int stoma_unindex(stoma_db_t *db,
	const char *field, const char *row_id);

/*
 * rec_ref_t-native convenience wrapper for stoma_unindex (same
 * id-uniformity boundary as stoma_index_ref): formats row_id as the
 * canonical decimal string internally. Same return contract as
 * stoma_unindex.
 */
int stoma_unindex_ref(stoma_db_t *db,
	const char *field, rec_ref_t row_id);

/*
 * The canonical single-facet field the store/unstore/readback adapters
 * below index under (RECALL-KERNEL.md, Phase 2A per-axis role "stoma"):
 * the whole value string's text. Multi-facet indexing stays raw native
 * (stoma_index/stoma_unindex per field).
 */
#define STOMA_AXIS_TEXT_FIELD "text"

/*
 * rec_axis_store / rec_axis_unstore / rec_axis_readback (Phase 2A store
 * contract, RECALL-KERNEL.md "rec_axis_store convention", optional
 * CLI-specific — not libqmap core API). The consumer passes (ref, value)
 * blindly; stoma stores the WHOLE value string's text under
 * STOMA_AXIS_TEXT_FIELD (replace-in-place: store erases any previous
 * entry for the ref first, so a ref owns exactly one doc).
 *   store    — 0 ok; -1 errno EINVAL (bad args / empty value); ctx NULL → -1.
 *   unstore  — removes the ref's entry; idempotent absent → 0; ctx NULL → -1.
 *   readback — one malloc'd buffer holding the stored (folded — lowercased,
 *              accent-preserving) text as one NUL-joined entry; n_out counts
 *              the display chars; absent → NULL/0, still 0; NULL out args → -1.
 * Not kernel API, not in the rec_axis_t vtable; dlsym'd like rec_axis_open.
 */
int rec_axis_store(void *ctx, const char *spec, rec_ref_t ref,
	const char *value);
int rec_axis_unstore(void *ctx, rec_ref_t ref);
int rec_axis_readback(void *ctx, rec_ref_t ref, char **blob_out,
	size_t *n_out);

/*
 * Query: every token of `query` must prefix-match in `field`.
 * Matching row_ids are stored as "row_id" → "" into out_hd (a caller-opened
 * qmap; duplicates collapse). *handled is set to 1 when the query produced
 * at least one token, 0 for an empty/zero-token query (no-op — caller should
 * treat it as "matches everything"). Returns the number of matches written.
 */
uint32_t stoma_query(stoma_db_t *db,
	const char *field, const char *query,
	uint32_t out_hd, int *handled);

/*
 * Phrase query: every token of `query` must prefix-match in `field`, AND the
 * tokens must occur in the indexed text as a contiguous subsequence (in query
 * order). Line breaks and punctuation are token separators, so a phrase may
 * span lines. A single-token query behaves exactly like stoma_query.
 * Contract is otherwise identical to stoma_query.
 */
uint32_t stoma_query_phrase(stoma_db_t *db,
	const char *field, const char *query,
	uint32_t out_hd, int *handled);

/*
 * Recall-kernel lexical filler (adapter contract, see rec.h): the exact set
 * of refs whose row_id matches `query` in `field`. The consumer must index
 * by its own decimal rec_ref_t (stoma_index(db, field, "42", value)) — the
 * filler pushes the parsed ids and stoma_rank reverses the mapping.
 * Semantics are identical to stoma_query (phrase=0) / stoma_query_phrase
 * (phrase=1). Refs are appended to `out` (additive) and the set is sealed
 * (0 = ok). -1 on NULL args or when a matched row_id is not strictly decimal
 * or cannot be represented as a rec_ref_t (> UINT32_MAX) (fill aborts; raw
 * entry points still serve non-numeric stores). Zero-token
 * or empty queries yield a sealed empty set (mirrors the handled=0 no-op).
 */
int rec_axis_fill_tokens(stoma_db_t *db,
	const char *field, const char *query,
	int phrase, rec_set_t *out);

/*
 * FTS score function for the recall-kernel rank loop: score = ctx->matched /
 * token_count of the folded field text of decimal(ref) (shorter docs rank
 * higher on ties). Caller must have indexed by its decimal ref. -1 on NULL
 * args, an unknown (field, row), or a zero-token document (the rank loop
 * skips the ref). The score may exceed 1.0 when matched > doc tokens.
 * Compatible with rec_score_fn via a caller adapter.
 */
struct stoma_rank_ctx {
	stoma_db_t *db;
	const char *field;
	size_t matched; /* matched query tokens (= query tokens for an AND-set) */
};
int stoma_rank(struct stoma_rank_ctx *ctx, rec_ref_t ref, float *score);

/*
 * Iterates non-whitespace word tokens in `text` and invokes cb(token, len, user).
 */
void stoma_tokenize(
        const char *folded, void (*cb)(const char *tok, size_t len, void *user),
        void *user);

/*
 * Line/token normalization utility: splits on newlines/CR, trims tokens,
 * and appends deduplicated non-empty tokens into `out` separated by '\n'.
 * Returns 0 on success, -1 on buffer overflow.
 */
int stoma_list_normalize(const char *input, char *out, size_t out_sz);

/*
 * Check if a newline-separated list contains `token`.
 * Returns 1 if present, 0 otherwise.
 */
int stoma_list_contains(const char *list, const char *token);

/*
 * Append a token to a newline-separated list if not already present.
 * Returns 0 on success, -1 on buffer overflow.
 */
int stoma_list_append(char *out, size_t out_sz, const char *token);

#endif
