# stoma — Search Tokenization and Accent-Sensitive Inverted Index

Fast in-memory inverted index and string tokenization library backed by `libcorm`.

## Overview

`stoma` powers the full-text search (FTS) engine for `hyle`. It builds an inverted index over `(field, token) -> row_id`.

## Key Features

- **Accent-Sensitive String Folding (`stoma_fold`):** Lowercases ASCII (A-Z) and Latin-1 supplement uppercase characters while strictly preserving diacritical marks (`pão` $\neq$ `pao`).
- **Prefix Matching:** Searches match word beginnings (e.g. query `cor` matches `coração`).
- **Contiguous Phrase Queries (`stoma_query_phrase`):** Matches multi-word sequences in exact token order across line breaks and punctuation.
- **Pure C / Zero External Encoding Dependencies:** Operates without `iconv` or system locale dependencies for fast, predictable execution.

## Key APIs (`include/stoma/stoma.h`)

```c
/* String lowercase folding (accent-preserving) */
int stoma_fold(char *out, size_t outsz, const char *in);

/* Open / close index */
stoma_db_t *stoma_open(unsigned mask);
void stoma_close(stoma_db_t *db);
void stoma_clear(stoma_db_t *db);

/* Index field value for record */
int stoma_index(stoma_db_t *db, const char *field, const char *row_id, const char *value);

/* rec_ref_t-native convenience: same as stoma_index but for callers whose
   id is already a rec_ref_t (the id-uniformity boundary shared with
   libjoint/libsepal/libislet's rec_axis_fill_* functions) -- formats the
   canonical decimal row_id internally. Storage/key format is unchanged;
   stoma_index's generic string row_id path is untouched. */
int stoma_index_ref(stoma_db_t *db, const char *field, rec_ref_t row_id, const char *value);

/* Differential inverse of stoma_index (Phase 2A): removes every posting
   plus the doc side-table entry for (field, row_id) by re-tokenizing the
   side-table text back to the exact posting keys. Cost is proportional to
   the row's own tokens, never O(store). Idempotent: absent (field,row)
   returns 0. -1 on invalid arguments. Zero new stored state. */
int stoma_unindex(stoma_db_t *db, const char *field, const char *row_id);

/* rec_ref_t-native convenience: same as stoma_unindex but for callers whose
   id is already a rec_ref_t -- formats the canonical decimal row_id
   internally. Same return contract as stoma_unindex. */
int stoma_unindex_ref(stoma_db_t *db, const char *field, rec_ref_t row_id);

/* Phase 2A store/unstore/readback adapters (RECALL-KERNEL.md, optional
   CLI-specific -- not libcorm core API). The consumer passes (ref, value)
   blindly; stoma stores the WHOLE value string's text under the canonical
   "text" field (replace-in-place: a ref owns exactly one doc). store: 0 ok,
   -1 errno EINVAL on bad args/empty value. unstore: removes the ref's entry,
   idempotent absent -> 0. readback: one malloc'd buffer with the stored
   (folded -- lowercased, accent-preserving) text as one NUL-joined entry,
   n_out counts the display chars; absent -> NULL/0, still 0. */
int rec_axis_store(void *ctx, const char *spec, rec_ref_t ref, const char *value);
int rec_axis_unstore(void *ctx, rec_ref_t ref);
int rec_axis_readback(void *ctx, rec_ref_t ref, char **blob_out, size_t *n_out);

/* Query index with token prefix matching */
uint32_t stoma_query(stoma_db_t *db, const char *field, const char *query,
                     uint32_t out_hd, int *handled);

/* Query index for exact contiguous phrases */
uint32_t stoma_query_phrase(stoma_db_t *db, const char *field, const char *query,
                            uint32_t out_hd, int *handled);

/* Word-token iterator used by index and query paths */
void stoma_tokenize(
        const char *folded, void (*cb)(const char *tok, size_t len, void *user),
        void *user);

/* Newline-separated token-list helpers (normalize / contains / append) */
int stoma_list_normalize(const char *input, char *out, size_t out_sz);
int stoma_list_contains(const char *list, const char *token);
int stoma_list_append(char *out, size_t out_sz, const char *token);
```

## Recall-kernel form

stoma is the lexical axis of the recall kernel (`rec.h` in libcorm; spec in
libcorm's `docs/RECALL-KERNEL.md`). Implemented adapter following the
contract (one filler, streams matches, seals, plain `int` return, additive):

```c
/* Exact lexical set: refs are the caller's decimal row ids. phrase=0 behaves
   as stoma_query, phrase=1 as stoma_query_phrase. Fills `out` additively and
   seals it; -1 on NULL args or a non-decimal row id encountered in the walk
   (the consumer must index by its own decimal rec_ref_t). Zero-token/empty
   queries yield a sealed empty set (mirrors the handled=0 no-op). */
int rec_axis_fill_tokens(stoma_db_t *db, const char *field,
                         const char *query, int phrase, rec_set_t *out);

/* FTS score ranker for the kernel loop: score = matched / token_count of the
   folded field text of decimal(ref). Shorter docs rank higher on ties.
   Composes as tokens(db, t) ∩ geo(b) ∩ time(r) → top-k via `rec_query_run`
   (libcorm docs/RECALL-KERNEL.md, "Running a query"). */
struct stoma_rank_ctx {
	stoma_db_t *db;
	const char *field;
	size_t matched; /* matched query tokens (= query tokens for an AND-set) */
};
int stoma_rank(struct stoma_rank_ctx *ctx, rec_ref_t ref, float *score);
```

`stoma_query`/`stoma_query_phrase` remain the raw entry points for
non-numeric row ids (hyle uses them today as a prefilter, no scoring); the
adapter is optional and additive.

## Dependencies

- `external/libcorm` — Hash map storage
