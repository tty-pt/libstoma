#ifndef STOMA_H
#define STOMA_H

/**
 * @file stoma.h
 * @brief stoma — corm-backed full-text index.
 *
 * A generic inverted index over (field, token) → row_id. It knows nothing
 * about rows, schemas, or the application: callers choose which fields to
 * index and interpret row_ids. Match semantics are word-token based with
 * prefix expansion: a query token must match the start of an indexed token.
 * Multiple tokens in a query are ANDed.
 */

#include <stddef.h>
#include <stdint.h>
#include <ttypt/rec.h>

/**
 * @brief Fold a UTF-8 string to lowercase, preserving accents.
 *
 * Accent-sensitive search: ASCII A-Z and the Latin-1 Supplement uppercase
 * letters are lowercased; all other bytes are copied verbatim. The fold is
 * locale-independent (no iconv, no setlocale) and never grows the output,
 * so it fits whenever outsz > strlen(in).
 *
 * @param[out] out   Output buffer.
 * @param[in]  outsz Capacity of @p out.
 * @param[in]  in    Input string.
 * @return Number of bytes written, or -1 if the output buffer is too
 *         small (the caller falls back to raw comparison).
 */
int stoma_fold(char *out, size_t outsz, const char *in);

/** @brief Opaque full-text index handle. */
typedef struct stoma_db stoma_db_t;

/**
 * @brief Open an index.
 * @param[in] mask Corm hash mask (0 = corm default).
 * @return New index handle, or NULL on failure.
 */
stoma_db_t *stoma_open(unsigned mask);

/**
 * @brief Open a stoma index from an opaque spec string.
 *
 * Follows the rec_axis_open convention (RECALL-KERNEL.md, optional
 * CLI-open convention, not part of libcorm's core rec_query registry
 * API). Returns the ctx a caller then passes to rec_axis_set_ctx() —
 * the stoma_db_t* directly. Two spec shapes (2B-2):
 *  - decimal mask (or empty/NULL): the stoma_open() mask, 0 = corm
 *    default — a plain in-memory index, nothing seeded.
 *  - a path (contains '/'): the 2B-2 primary-seeded rebuild. The
 *    directory is scanned for a `<primary>.roster` sidecar (written by
 *    the corm CLI's roster persistence); the primary corm store it
 *    prefixes is opened (`:a:s` raw and `:a:u` decimal alike) and every
 *    record is re-indexed under the canonical `text` field. A rebuild
 *    emits a one-line budget note (docs + ms) on stderr so callers can
 *    measure (mm-plan U4). A path whose sidecar is missing or whose
 *    primary cannot be opened warns loudly and falls back to an empty
 *    memory index — it never silently serves stale or empty results.
 *
 * @param[in] spec Spec string as described above.
 * @return A stoma_db_t* usable as the rec_axis ctx, or NULL on failure.
 */
void *rec_axis_open(const char *spec);

/**
 * @brief Close the index and free all resources.
 * @param[in] db Index handle.
 */
void stoma_close(stoma_db_t *db);

/**
 * @brief Drop all entries; the handle stays valid.
 * @param[in] db Index handle.
 */
void stoma_clear(stoma_db_t *db);

/**
 * @brief Index every token of @p value under (field, row_id).
 *
 * Duplicate tokens collapse automatically.
 *
 * @param[in] db     Index handle.
 * @param[in] field  Field name.
 * @param[in] row_id Row id string.
 * @param[in] value  Text to tokenize and index.
 * @return 0 on success, -1 on invalid arguments.
 */
int stoma_index(stoma_db_t *db,
	const char *field, const char *row_id, const char *value);

/**
 * @brief rec_ref_t-native convenience wrapper for stoma_index.
 *
 * Shared with libjoint/libsepal/libislet's rec_axis_fill_* functions at
 * the id-uniformity boundary: formats row_id as the canonical decimal
 * string stoma_rank/rec_axis_fill_tokens expect and calls stoma_index.
 * This does not change stoma's storage or key format, and does not
 * remove the generic string-row_id path (stoma_index proper) — it only
 * saves every rec_ref_t-keyed caller from doing its own decimal
 * formatting before indexing. Same return contract as stoma_index.
 *
 * @param[in] db     Index handle.
 * @param[in] field  Field name.
 * @param[in] row_id Row ref.
 * @param[in] value  Text to tokenize and index.
 * @return 0 on success, -1 on invalid arguments.
 */
int stoma_index_ref(stoma_db_t *db,
	const char *field, rec_ref_t row_id, const char *value);

/**
 * @brief Remove every posting for (field, row_id) plus the doc side-table.
 *
 * The differential inverse of stoma_index (Phase 2A `rec_axis_unstore`
 * mechanism; RECALL-KERNEL.md "rec_axis_store convention"): index →
 * unindex leaves zero residual postings for the row and no doc entry, at
 * a cost ∝ tokens of the row (never O(store), never a scan — the doc
 * side-table is walked backwards to recover the exact posting keys).
 * Idempotent: an absent (field, row) reads as 0, not an error.
 * Zero new stored state; memory-only.
 *
 * @param[in] db     Index handle.
 * @param[in] field  Field name.
 * @param[in] row_id Row id string.
 * @return 0 on success, -1 on invalid arguments.
 */
int stoma_unindex(stoma_db_t *db,
	const char *field, const char *row_id);

/**
 * @brief rec_ref_t-native convenience wrapper for stoma_unindex.
 *
 * Same id-uniformity boundary as stoma_index_ref: formats row_id as the
 * canonical decimal string internally. Same return contract as
 * stoma_unindex.
 *
 * @param[in] db     Index handle.
 * @param[in] field  Field name.
 * @param[in] row_id Row ref.
 * @return 0 on success, -1 on invalid arguments.
 */
int stoma_unindex_ref(stoma_db_t *db,
	const char *field, rec_ref_t row_id);

/**
 * @brief Canonical single-facet field for the store/unstore/readback adapters.
 *
 * RECALL-KERNEL.md, Phase 2A per-axis role "stoma": the whole value
 * string's text. Multi-facet indexing stays raw native (stoma_index /
 * stoma_unindex per field).
 */
#define STOMA_AXIS_TEXT_FIELD "text"

/**
 * @brief Store a ref's whole value text (rec_axis_store convention).
 *
 * Phase 2A store contract (RECALL-KERNEL.md "rec_axis_store convention",
 * optional CLI-specific — not libcorm core API). The consumer passes
 * (ref, value) blindly; stoma stores the WHOLE value string's text under
 * STOMA_AXIS_TEXT_FIELD (replace-in-place: store erases any previous
 * entry for the ref first, so a ref owns exactly one doc). Not kernel
 * API, not in the rec_axis_t vtable; dlsym'd like rec_axis_open.
 *
 * @param[in] ctx   Index handle (the stoma_db_t*).
 * @param[in] spec  Reserved — always NULL.
 * @param[in] ref   Row ref.
 * @param[in] value Whole value string to index.
 * @return 0 ok; -1 with errno=EINVAL on bad args/empty value; ctx NULL → -1.
 */
int rec_axis_store(void *ctx, const char *spec, rec_ref_t ref,
	const char *value);

/**
 * @brief Remove a ref's stored doc (rec_axis_unstore).
 *
 * Removes the ref's entry under STOMA_AXIS_TEXT_FIELD; idempotent:
 * absent → 0. Not kernel API, not in the rec_axis_t vtable; dlsym'd
 * like rec_axis_open.
 *
 * @param[in] ctx Index handle (the stoma_db_t*).
 * @param[in] ref Row ref.
 * @return 0 on success; ctx NULL → -1.
 */
int rec_axis_unstore(void *ctx, rec_ref_t ref);

/**
 * @brief Read a ref's stored (folded) text (rec_axis_readback).
 *
 * One malloc'd buffer holding the stored (folded — lowercased,
 * accent-preserving) text as one NUL-joined entry; n_out counts the
 * display chars; absent → NULL/0, still 0; NULL out args → -1. Not
 * kernel API, not in the rec_axis_t vtable; dlsym'd like rec_axis_open.
 *
 * @param[in]  ctx      Index handle (the stoma_db_t*).
 * @param[in]  ref      Row ref.
 * @param[out] blob_out Receives the malloc'd text buffer (caller frees).
 * @param[out] n_out    Receives the display char count.
 * @return 0 on success, -1 on error.
 */
int rec_axis_readback(void *ctx, rec_ref_t ref, char **blob_out,
	size_t *n_out);

/**
 * @brief Query: every token of @p query must prefix-match in @p field.
 *
 * Matching row_ids are stored as "row_id" → "" into out_hd (a
 * caller-opened corm; duplicates collapse). *handled is set to 1 when
 * the query produced at least one token, 0 for an empty/zero-token query
 * (no-op — the caller should treat it as "matches everything").
 *
 * @param[in]  db      Index handle.
 * @param[in]  field   Field name.
 * @param[in]  query   Query string.
 * @param[in]  out_hd  Caller-opened corm handle for result rows.
 * @param[out] handled 1 when the query had ≥1 token, else 0.
 * @return Number of matches written.
 */
uint32_t stoma_query(stoma_db_t *db,
	const char *field, const char *query,
	uint32_t out_hd, int *handled);

/**
 * @brief Phrase query: prefix-match with contiguous subsequence ordering.
 *
 * Every token of @p query must prefix-match in @p field, AND the tokens
 * must occur in the indexed text as a contiguous subsequence (in query
 * order). Line breaks and punctuation are token separators, so a phrase
 * may span lines. A single-token query behaves exactly like stoma_query.
 * The contract is otherwise identical to stoma_query.
 *
 * @param[in]  db      Index handle.
 * @param[in]  field   Field name.
 * @param[in]  query   Query string.
 * @param[in]  out_hd  Caller-opened corm handle for result rows.
 * @param[out] handled 1 when the query had ≥1 token, else 0.
 * @return Number of matches written.
 */
uint32_t stoma_query_phrase(stoma_db_t *db,
	const char *field, const char *query,
	uint32_t out_hd, int *handled);

/**
 * @brief Recall-kernel lexical filler (adapter contract, see rec.h).
 *
 * The exact set of refs whose row_id matches @p query in @p field. The
 * consumer must index by its own decimal rec_ref_t
 * (stoma_index(db, field, "42", value)) — the filler pushes the parsed
 * ids and stoma_rank reverses the mapping. Semantics are identical to
 * stoma_query (phrase=0) / stoma_query_phrase (phrase=1). Refs are
 * appended to @p out (additive) and the set is sealed (0 = ok).
 *
 * @param[in]  db     Index handle.
 * @param[in]  field  Field name.
 * @param[in]  query  Query string.
 * @param[in]  phrase Non-zero for phrase semantics.
 * @param[out] out    rec_set_t to append matching refs to.
 * @return 0 ok; -1 on NULL args or when a matched row_id is not strictly
 *         decimal or cannot be represented as a rec_ref_t (> UINT32_MAX)
 *         (fill aborts; raw entry points still serve non-numeric stores).
 *         Zero-token or empty queries yield a sealed empty set (mirrors
 *         the handled=0 no-op).
 */
int rec_axis_fill_tokens(stoma_db_t *db,
	const char *field, const char *query,
	int phrase, rec_set_t *out);

/**
 * @brief Context for the stoma_rank FTS score function.
 */
struct stoma_rank_ctx {
	/** Index handle. */
	stoma_db_t *db;
	/** Field name to score against. */
	const char *field;
	/** Matched query tokens (= query tokens for an AND-set). */
	size_t matched;
};

/**
 * @brief FTS score function for the recall-kernel rank loop.
 *
 * score = ctx->matched / token_count of the folded field text of
 * decimal(ref) (shorter docs rank higher on ties). The caller must have
 * indexed by its decimal ref. Compatible with rec_score_fn via a caller
 * adapter. The score may exceed 1.0 when matched > doc tokens.
 *
 * @param[in]  ctx   Rank context.
 * @param[in]  ref   Row ref.
 * @param[out] score Receives the score.
 * @return 0 ok; -1 on NULL args, an unknown (field, row), or a zero-token
 *         document (the rank loop skips the ref).
 */
int stoma_rank(struct stoma_rank_ctx *ctx, rec_ref_t ref, float *score);

/**
 * @brief Tokenize non-whitespace word tokens in @p folded.
 *
 * Iterates non-whitespace word tokens in @p folded and invokes
 * cb(token, len, user) for each.
 *
 * @param[in] folded Folded text to tokenize.
 * @param[in] cb     Callback invoked per token.
 * @param[in] user   Opaque user pointer passed to @p cb.
 */
void stoma_tokenize(
        const char *folded, void (*cb)(const char *tok, size_t len, void *user),
        void *user);

/**
 * @brief Normalize a newline-separated token list.
 *
 * Splits on newlines/CR, trims tokens, and appends deduplicated
 * non-empty tokens into @p out separated by '\n'.
 *
 * @param[in]  input  Input list.
 * @param[out] out    Destination buffer.
 * @param[in]  out_sz Capacity of @p out.
 * @return 0 on success, -1 on buffer overflow.
 */
int stoma_list_normalize(const char *input, char *out, size_t out_sz);

/**
 * @brief Check if a newline-separated list contains @p token.
 * @param[in] list  Newline-separated list.
 * @param[in] token Token to find.
 * @return 1 if present, 0 otherwise.
 */
int stoma_list_contains(const char *list, const char *token);

/**
 * @brief Append a token to a newline-separated list if not already present.
 * @param[out] out    Destination list buffer.
 * @param[in]  out_sz Capacity of @p out.
 * @param[in]  token  Token to append.
 * @return 0 on success, -1 on buffer overflow.
 */
int stoma_list_append(char *out, size_t out_sz, const char *token);

#endif