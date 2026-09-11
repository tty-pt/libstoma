# stoma — Search Tokenization and Accent-Sensitive Inverted Index

Fast in-memory inverted index and string tokenization library backed by `libqmap`.

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

/* Query index with token prefix matching */
uint32_t stoma_query(stoma_db_t *db, const char *field, const char *query,
                     uint32_t out_hd, int *handled);

/* Query index for exact contiguous phrases */
uint32_t stoma_query_phrase(stoma_db_t *db, const char *field, const char *query,
                            uint32_t out_hd, int *handled);
```

## Recall-kernel form

stoma is the lexical axis of the recall kernel (`rec.h` in libqmap; spec in
libqmap's `docs/RECALL-KERNEL.md`). Implemented adapter following the
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
   Proposed consumer plan (rec_query R4): tokens(db, t) ∩ geo(b) ∩ time(r) →
   soonest+FTS → top-k, composed via `rec_query_run` (PLAN-REC-QUERY §2). */
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

- `external/libqmap` — Hash map storage
