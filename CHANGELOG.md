## 1.0.0

- **First stable release** — search tokenization and accent-sensitive inverted index for hyle's full-text search, backed by `libcorm`.
- **Accent-preserving folding**: `stoma_fold` lowercases ASCII (A-Z) and Latin-1 supplement uppercase while strictly preserving diacritics — `pão` ≠ `pao`.
- **Inverted index**: `stoma_open`/`stoma_close`/`stoma_clear`, `stoma_index`/`stoma_unindex` (differential inverse proportional to the row's own tokens, never O(store), idempotent) over `(field, token) → row_id`.
- **`rec_ref_t`-native conveniences**: `stoma_index_ref`/`stoma_unindex_ref` for callers whose id is already a `rec_ref_t`.
- **Prefix and phrase queries**: `stoma_query` (word-beginning match — `cor` matches `coração`) and `stoma_query_phrase` (contiguous multi-word sequences in exact token order across line breaks and punctuation), both with `uint32_t` row handles.
- **Recall-kernel form**: `rec_axis_fill_tokens` (exact lexical set, `phrase` flag, additive fill + seal) and the `stoma_rank` FTS scorer (score = matched / token_count of the folded field text, shorter docs rank higher on ties) — composes as tokens ∩ geo ∩ time → top-k via `rec_query_run`.
- **Recall-kernel storage adapters**: `rec_axis_store` / `rec_axis_unstore` / `rec_axis_readback` (replace-in-place, one ref owns one doc; folded text as a NUL-joined blob).
- **Token utilities**: `stoma_tokenize` word iterator, `stoma_list_normalize`/`stoma_list_contains`/`stoma_list_append` newline-separated helpers.
- **Pure C, zero encoding dependencies**: no `iconv` or system locale — predictable execution.
