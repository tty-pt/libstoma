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
 * rec_axis_open (PLAN-REC-QUERY.md §4.3, optional CLI-open convention,
 * not part of libqmap's core rec_query registry API): opens a stoma
 * index from an opaque spec string (the decimal stoma_open() mask;
 * empty/NULL -> 0, the qmap default) and returns the ctx a caller then
 * passes to rec_axis_set_ctx() -- the stoma_db_t* directly.
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
 * (fill aborts; raw entry points still serve non-numeric stores). Zero-token
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
