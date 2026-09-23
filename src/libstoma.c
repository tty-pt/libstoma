#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <libgen.h>
#include <sys/stat.h>
#include <time.h>
#include <ttypt/corm.h>
#include "stoma/stoma.h"

#define STOMA_MAX_TOKENS 64

/* Declared in token.c (internal). */
void stoma_tokenize(
        const char *folded, void (*cb)(const char *tok, size_t len, void *user),
        void *user);

struct stoma_db {
	unsigned hd;     /* CM_SORTED, CM_STR → CM_STR inverted index */
	unsigned doc_hd; /* CM_SORTED, CM_STR → CM_STR folded text per
	                    (field,row) */
};

/* D14 axis-contributed CLI option state (rec_axis_config_arg, round 2).
 * decode merges it into the bare/empty spec path (searchable when --query
 * is present; field defaults to "text") and fills fields a non-empty leaf
 * omits. */
static char *stoma_cli_query;
static char *stoma_cli_field;
static int stoma_cli_phrase;
static size_t stoma_cli_matched;
static int stoma_cli_field_set, stoma_cli_phrase_set, stoma_cli_matched_set;

stoma_db_t *stoma_open(unsigned mask)
{
	stoma_db_t *db;

	db = calloc(1, sizeof(*db));
	if (!db)
		return NULL;
	db->hd = corm_open(NULL, NULL, CM_STR, CM_STR, mask, CM_SORTED);
	if (!db->hd) {
		free(db);
		return NULL;
	}
	db->doc_hd = corm_open(NULL, NULL, CM_STR, CM_STR, mask, CM_SORTED);
	if (!db->doc_hd) {
		corm_close(db->hd);
		free(db);
		return NULL;
	}
	return db;
}

void stoma_close(stoma_db_t *db)
{
	if (!db)
		return;
	corm_close(db->hd);
	corm_close(db->doc_hd);
	free(db);
}

void stoma_clear(stoma_db_t *db)
{
	if (!db)
		return;
	corm_drop(db->hd);
	corm_drop(db->doc_hd);
}

/* ---- index ---- */

typedef struct {
	const char *field;
	const char *row_id;
	unsigned hd;
} index_ctx_t;

static void index_token(const char *tok, size_t len, void *user)
{
	index_ctx_t *ctx = (index_ctx_t *)user;
	size_t fld = strlen(ctx->field);
	size_t rid = strlen(ctx->row_id);
	char *key;

	key = malloc(fld + rid + len + 3);
	if (!key)
		return;
	memcpy(key, ctx->field, fld);
	key[fld] = '\t';
	memcpy(key + fld + 1, tok, len);
	key[fld + 1 + len] = '\t';
	memcpy(key + fld + 2 + len, ctx->row_id, rid);
	key[fld + 2 + len + rid] = '\0';
	corm_put(ctx->hd, key, "");
	free(key);
}

int stoma_index(
        stoma_db_t *db, const char *field, const char *row_id,
        const char *value)
{
	index_ctx_t ctx;
	size_t vlen;
	char *folded;

	if (!db || !field || !row_id || !value)
		return -1;
	/* The fold never grows its output, so strlen+1 always fits. */
	vlen = strlen(value);
	folded = malloc(vlen + 1);
	if (!folded)
		return -1;
	if (stoma_fold(folded, vlen + 1, value) < 0) {
		free(folded);
		return 0;
	}
	ctx.field = field;
	ctx.row_id = row_id;
	ctx.hd = db->hd;
	stoma_tokenize(folded, index_token, &ctx);

	/* Side table: store the folded text per (field,row_id) so phrase
	 * queries can re-tokenize a document and test token adjacency. */
	{
		size_t fld = strlen(field);
		size_t rid = strlen(row_id);
		size_t dlen = fld + 1 + rid;
		char *dkey = malloc(dlen + 1);
		size_t fl = strlen(folded);
		char *fdoc = malloc(fl + 1);

		if (dkey && fdoc) {
			memcpy(dkey, field, fld);
			dkey[fld] = '\t';
			memcpy(dkey + fld + 1, row_id, rid);
			dkey[dlen] = '\0';
			memcpy(fdoc, folded, fl + 1);
			corm_put(db->doc_hd, dkey, fdoc);
		}
		free(dkey);
		free(fdoc);
	}

	free(folded);
	return 0;
}

int stoma_index_ref(
        stoma_db_t *db, const char *field, rec_ref_t row_id,
        const char *value)
{
	char rid[24];

	snprintf(rid, sizeof(rid), "%llu", (unsigned long long)row_id);
	return stoma_index(db, field, rid, value);
}

/* ---- unindex (the differential inverse of index) ---- */

typedef struct {
	const char *tok;
	size_t len;
} unindex_tok_t;

typedef struct {
	unindex_tok_t *ent;
	size_t n;
	size_t cap;
} unindex_toks_t;

static void collect_unindex_token(const char *tok, size_t len, void *user)
{
	unindex_toks_t *t = (unindex_toks_t *)user;

	if (t->n == t->cap) {
		size_t ncap = t->cap ? t->cap * 2 : 64;
		unindex_tok_t *nt = realloc(t->ent, ncap * sizeof(*nt));

		if (!nt)
			return;
		t->ent = nt;
		t->cap = ncap;
	}
	t->ent[t->n].tok = tok;
	t->ent[t->n].len = len;
	t->n++;
}

int stoma_unindex(stoma_db_t *db, const char *field, const char *row_id)
{
	size_t fld;
	size_t rid;
	size_t dlen;
	char *dkey;
	const char *dtext;
	unindex_toks_t toks;
	size_t i;

	if (!db || !field || !row_id)
		return -1;
	/* The posting keys only ever encode the doc's own tokens, so the
	 * side-table text walks backwards to the exact keys index wrote. */
	fld = strlen(field);
	rid = strlen(row_id);
	dlen = fld + 1 + rid;
	dkey = malloc(dlen + 1);
	if (!dkey)
		return -1;
	memcpy(dkey, field, fld);
	dkey[fld] = '\t';
	memcpy(dkey + fld + 1, row_id, rid);
	dkey[dlen] = '\0';
	dtext = (const char *)corm_get(db->doc_hd, dkey);
	if (!dtext) {
		free(dkey);
		return 0; /* absent (field,row): idempotent no-op */
	}
	memset(&toks, 0, sizeof(toks));
	stoma_tokenize(dtext, collect_unindex_token, &toks);
	/* Duplicate tokens collapsed at index time (same key replaces),
	 * so deleting each occurrence's key is exact; repeats are
	 * harmless no-ops. */
	for (i = 0; i < toks.n; i++) {
		size_t tlen = toks.ent[i].len;
		size_t klen = fld + 1 + tlen + 1 + rid;
		char *key = malloc(klen + 1);

		if (!key)
			continue;
		memcpy(key, field, fld);
		key[fld] = '\t';
		memcpy(key + fld + 1, toks.ent[i].tok, tlen);
		key[fld + 1 + tlen] = '\t';
		memcpy(key + fld + 2 + tlen, row_id, rid);
		key[klen] = '\0';
		corm_del(db->hd, key);
		free(key);
	}
	free(toks.ent);
	corm_del(db->doc_hd, dkey);
	free(dkey);
	return 0;
}

int stoma_unindex_ref(stoma_db_t *db, const char *field, rec_ref_t row_id)
{
	char rid[24];

	snprintf(rid, sizeof(rid), "%llu", (unsigned long long)row_id);
	return stoma_unindex(db, field, rid);
}

/* ---- rec_axis store / unstore / readback (Phase 2A) ---- */

int rec_axis_store(void *ctx, const char *spec, rec_ref_t ref,
	const char *value)
{
	stoma_db_t *db = ctx;

	(void)spec; /* reserved — NULL */
	if (!db || !value) {
		errno = EINVAL;
		return -1;
	}
	if (!*value) { /* an empty string indexes nothing: loud reject */
		errno = EINVAL;
		return -1;
	}
	/* Replace-in-place: the ref owns exactly one doc, so erase any
	 * previous entry first (idempotent when absent). */
	if (stoma_unindex_ref(db, STOMA_AXIS_TEXT_FIELD, ref) != 0)
		return -1;
	return stoma_index_ref(db, STOMA_AXIS_TEXT_FIELD, ref, value);
}

int rec_axis_unstore(void *ctx, rec_ref_t ref)
{
	stoma_db_t *db = ctx;

	if (!db) {
		errno = EINVAL;
		return -1;
	}
	/* stoma_unindex is already absent → 0; the idempotent contract. */
	return stoma_unindex_ref(db, STOMA_AXIS_TEXT_FIELD, ref);
}

int rec_axis_readback(void *ctx, rec_ref_t ref, char **blob_out,
	size_t *n_out)
{
	stoma_db_t *db = ctx;
	char rid[24];
	size_t fld;
	size_t rlen;
	char *dkey;
	const char *dtext;
	char *copy;

	if (blob_out)
		*blob_out = NULL;
	if (n_out)
		*n_out = 0;
	if (!db || !blob_out || !n_out) {
		errno = EINVAL;
		return -1;
	}
	/* One entry: the stored (folded) doc text. The store keeps no
	 * other copy, so read-back is exactly what queries see. */
	snprintf(rid, sizeof(rid), "%llu", (unsigned long long)ref);
	fld = strlen(STOMA_AXIS_TEXT_FIELD);
	rlen = strlen(rid);
	dkey = malloc(fld + 1 + rlen + 1);
	if (!dkey)
		return -1;
	memcpy(dkey, STOMA_AXIS_TEXT_FIELD, fld);
	dkey[fld] = '\t';
	memcpy(dkey + fld + 1, rid, rlen);
	dkey[fld + 1 + rlen] = '\0';
	dtext = (const char *)corm_get(db->doc_hd, dkey);
	free(dkey);
	if (!dtext)
		return 0; /* absent → NULL/0, still 0 */
	copy = strdup(dtext);
	if (!copy)
		return -1;
	*blob_out = copy;
	*n_out = strlen(copy);
	return 0;
}

/* ---- query ---- */

typedef struct {
	const char *toks[STOMA_MAX_TOKENS];
	size_t len[STOMA_MAX_TOKENS];
	size_t n;
} collect_ctx_t;

static void collect_token(const char *tok, size_t len, void *user)
{
	collect_ctx_t *c = (collect_ctx_t *)user;

	if (c->n < STOMA_MAX_TOKENS) {
		c->toks[c->n] = tok;
		c->len[c->n] = len;
		c->n++;
	}
}

/* ---- phrase verification ---- */

typedef struct {
	const char *tok;
	size_t len;
} dtok_t;

typedef struct {
	dtok_t *ent;
	size_t n;
	size_t cap;
} dctoks_t;

static void collect_doc_token(const char *tok, size_t len, void *user)
{
	dctoks_t *t = (dctoks_t *)user;

	if (t->n == t->cap) {
		size_t ncap = t->cap ? t->cap * 2 : 64;
		dtok_t *nt = realloc(t->ent, ncap * sizeof(*nt));

		if (!nt)
			return;
		t->ent = nt;
		t->cap = ncap;
	}
	t->ent[t->n].tok = tok;
	t->ent[t->n].len = len;
	t->n++;
}

/* Contiguous-subsequence test: the query tokens must appear in order with
 * per-token prefix matching. A single query token is trivially present. */
static int doc_matches_phrase(const dctoks_t *d, const collect_ctx_t *q)
{
	size_t i;
	size_t j;

	if (q->n > d->n)
		return 0;
	if (q->n == 1)
		return 1;
	for (i = 0; i + q->n <= d->n; i++) {
		for (j = 0; j < q->n; j++) {
			if (d->ent[i + j].len < q->len[j])
				break;
			if (memcmp(d->ent[i + j].tok, q->toks[j], q->len[j]) !=
			    0)
				break;
		}
		if (j == q->n)
			return 1;
	}
	return 0;
}

/* ---- recall-kernel adapter ---- */

typedef struct {
	unsigned hd;    /* raw-path output handle            */
	rec_set_t *set; /* kernel candidate set              */
	int err;        /* strict decimal-parse failure flag */
} stoma_dest_t;

static void stoma_dest_emit(stoma_dest_t *d, const char *row_id)
{
	if (d->hd) {
		corm_put(d->hd, row_id, "");
		return;
	}
	if (d->set) {
		char *end;
		unsigned long long v;

		errno = 0;
		if (row_id[0] < '0' || row_id[0] > '9') {
			d->err = 1;
			return;
		}
		v = strtoull(row_id, &end, 10);
		if (!end || *end != '\0' || errno == ERANGE) {
			d->err = 1;
			return;
		}
		if (v > UINT32_MAX) { /* rec_ref_t is u32: never alias upward */
			d->err = 1;
			return;
		}
		rec_set_push(d->set, (rec_ref_t)v);
	}
}

static uint32_t stoma_query_any(
        stoma_db_t *db, const char *field, const char *query, stoma_dest_t *out,
        int *handled, int phrase)
{
	collect_ctx_t toks;
	char fold_stack[256];
	char *folded;
	char *prefix = NULL;
	char *dkey = NULL;
	size_t dcap = 0;
	size_t fld;
	size_t qlen;
	size_t pw = 0;
	unsigned cur_hd = 0;
	uint32_t matches = 0;
	size_t i;

	if (handled)
		*handled = 0;
	if (!db || !field || !query || !out)
		return 0;
	if (!out->hd && !out->set)
		return 0;
	/* The fold never grows its output, so strlen+1 always fits. Short
	 * queries fold into the stack buffer; long ones spill to the heap. */
	qlen = strlen(query);
	if (qlen + 1 <= sizeof(fold_stack)) {
		folded = fold_stack;
	} else {
		folded = malloc(qlen + 1);
		if (!folded)
			return 0;
	}
	if (stoma_fold(folded, qlen + 1, query) < 0) {
		if (folded != fold_stack)
			free(folded);
		return 0;
	}

	toks.n = 0;
	stoma_tokenize(folded, collect_token, &toks);
	if (toks.n == 0) {
		if (folded != fold_stack)
			free(folded);
		return 0;
	}
	if (handled)
		*handled = 1;

	fld = strlen(field);
	/* One prefix buffer for every token: the longest token cannot exceed
	 * the folded query, so a single sizing pass caps the allocation. */
	for (i = 0; i < toks.n; i++)
		if (toks.len[i] > pw)
			pw = toks.len[i];
	prefix = malloc(fld + pw + 2); /* field '\t' token NUL */
	if (!prefix) {
		if (folded != fold_stack)
			free(folded);
		return 0;
	}
	memcpy(prefix, field, fld);
	prefix[fld] = '\t';

	for (i = 0; i < toks.n; i++) {
		unsigned nxt_hd;
		size_t plen = fld + 1 + toks.len[i];
		uint32_t cur;
		const void *k;
		const void *v;

		memcpy(prefix + fld + 1, toks.toks[i], toks.len[i]);
		prefix[plen] = '\0';

		nxt_hd = corm_open(NULL, NULL, CM_STR, CM_STR, 0xFF, 0);
		if (!nxt_hd)
			break;

		/* Prefix scan: index map is CM_SORTED, CM_RANGE iterates from
		 * the lower bound to the end; break once the prefix no longer
		 * matches (contiguous keys). */
		cur = corm_iter(db->hd, prefix, CM_RANGE);
		while (corm_next(&k, &v, cur)) {
			const char *key = (const char *)k;
			const char *sep1;
			const char *sep2;

			if (strncmp(key, prefix, plen) != 0)
				break;
			sep1 = strchr(key, '\t');
			if (!sep1)
				continue;
			sep2 = strchr(sep1 + 1, '\t');
			if (!sep2)
				continue;
			if (!cur_hd || corm_get(cur_hd, sep2 + 1))
				corm_put(nxt_hd, sep2 + 1, "");
		}
		corm_fin(cur);

		if (cur_hd)
			corm_close(cur_hd);
		cur_hd = nxt_hd;
	}

	if (cur_hd) {
		uint32_t cur = corm_iter(cur_hd, NULL, 0);
		const void *k;
		const void *v;

		while (corm_next(&k, &v, cur)) {
			const char *rid = (const char *)k;
			int keep = 1;

			if (phrase && toks.n > 1) {
				size_t rl = strlen(rid);
				size_t dfl = fld + 1 + rl;
				const char *dtext;
				dctoks_t d;

				if (dfl + 1 > dcap) {
					char *nt = realloc(dkey, dfl + 1);
					if (!nt)
						break;
					dkey = nt;
					dcap = dfl + 1;
				}
				memcpy(dkey, field, fld);
				dkey[fld] = '\t';
				memcpy(dkey + fld + 1, rid, rl);
				dkey[dfl] = '\0';
				dtext = (const char *)corm_get(db->doc_hd, dkey);
				memset(&d, 0, sizeof(d));
				if (dtext) {
					stoma_tokenize(
					        dtext, collect_doc_token, &d);
					keep = doc_matches_phrase(&d, &toks);
				} else {
					keep = 0;
				}
				free(d.ent);
			}
			if (keep) {
				stoma_dest_emit(out, rid);
				matches++;
			}
		}
		corm_fin(cur);
		corm_close(cur_hd);
	}

	if (folded != fold_stack)
		free(folded);
	free(prefix);
	free(dkey);

	return matches;
}

uint32_t stoma_query(
        stoma_db_t *db, const char *field, const char *query, uint32_t out_hd,
        int *handled)
{
	stoma_dest_t d = { out_hd, NULL, 0 };

	return stoma_query_any(db, field, query, &d, handled, 0);
}

uint32_t stoma_query_phrase(
        stoma_db_t *db, const char *field, const char *query, uint32_t out_hd,
        int *handled)
{
	stoma_dest_t d = { out_hd, NULL, 0 };

	return stoma_query_any(db, field, query, &d, handled, 1);
}

/* ---- recall-kernel fill + rank ---- */

int rec_axis_fill_tokens(stoma_db_t *db, const char *field, const char *query,
                         int phrase, rec_set_t *out)
{
	stoma_dest_t d;

	if (!db || !field || !query || !out)
		return -1;
	d.hd = 0;
	d.set = out;
	d.err = 0;
	stoma_query_any(db, field, query, &d, NULL, phrase ? 1 : 0);
	if (d.err)
		return -1;
	rec_set_seal(out);
	return 0;
}

int stoma_rank(struct stoma_rank_ctx *ctx, rec_ref_t ref, float *score)
{
	char rid[24];
	size_t fld;
	size_t rlen;
	char *dkey;
	const char *dtext;
	dctoks_t d;

	if (!ctx || !ctx->db || !ctx->field || !score)
		return -1;
	snprintf(rid, sizeof(rid), "%llu", (unsigned long long)ref);
	fld = strlen(ctx->field);
	rlen = strlen(rid);
	dkey = malloc(fld + 1 + rlen + 1);
	if (!dkey)
		return -1;
	memcpy(dkey, ctx->field, fld);
	dkey[fld] = '\t';
	memcpy(dkey + fld + 1, rid, rlen);
	dkey[fld + 1 + rlen] = '\0';
	dtext = (const char *)corm_get(ctx->db->doc_hd, dkey);
	free(dkey);
	if (!dtext)
		return -1;
	memset(&d, 0, sizeof(d));
	stoma_tokenize(dtext, collect_doc_token, &d);
	if (d.n == 0) {
		free(d.ent);
		return -1;
	}
	*score = (float)ctx->matched / (float)d.n;
	free(d.ent);
	return 0;
}

/* ---- rec_query axis registration (stoma) ---- */

struct rec_stoma_params {
	const char *field;
	const char *query;
	int         phrase;
	size_t      matched; /* rank param: matched query tokens */
};

static int stoma_axis_fill(void *ctx, void *params, rec_set_t *out)
{
	stoma_db_t *db = ctx;
	const struct rec_stoma_params *p = params;

	if (!p)
		return -1;
	return rec_axis_fill_tokens(db, p->field, p->query, p->phrase, out);
}

static int stoma_axis_rank(void *ctx, void *params, rec_ref_t ref, float *score)
{
	const struct rec_stoma_params *p = params;
	struct stoma_rank_ctx sc;

	if (!p)
		return -1;
	sc.db = ctx;
	sc.field = p->field;
	sc.matched = p->matched;
	return stoma_rank(&sc, ref, score);
}

/*
 * Decode "field=body query='hello world' phrase=1 matched=2" into a
 * heap-owned rec_stoma_params. query/field default to "", phrase/
 * matched default to 0. Single-quoted values may contain spaces. The
 * decode-spec grammar is kernel-owned (ttypt/rec.h rec_spec_scan):
 * values are parsed in place with bounded parsers, so decode allocates
 * nothing until at most two owned string copies (field/query when the
 * leaf provides a non-empty value). The kept strings are owned copies;
 * CLI fallbacks are borrowed process-lifetime globals.
 */
static void *stoma_axis_decode(const char *s)
{
	struct rec_stoma_params *p;
	const char *q_leaf = NULL, *f_leaf = NULL;
	size_t qlen = 0, flen = 0;
	char *q_owned = NULL, *f_owned = NULL;
	int has_query = 0, has_field = 0, has_phrase = 0, has_matched = 0;
	int q_quoted = 0, f_quoted = 0;

	p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->field = STOMA_AXIS_TEXT_FIELD;
	p->query = "";
	p->phrase = 0;
	p->matched = 0;

	/* bare spec: searchable only when a CLI --query is present (field
	 * defaults to "text" — the documented store field, so there is no
	 * field="" hazard). Otherwise NULL (loud fill failure, unchanged). */
	if (!s) {
		if (!stoma_cli_query) {
			free(p);
			return NULL;
		}
		p->field = stoma_cli_field_set ? stoma_cli_field
		                              : STOMA_AXIS_TEXT_FIELD;
		p->query = stoma_cli_query;
		p->phrase = stoma_cli_phrase;
		p->matched = stoma_cli_matched;
		return p;
	}

	/* empty spec stays a degenerate query (query="" -> no matches) */
	if (!*s)
		return p;

	{
		const char *cur = s, *key, *val;
		size_t klen, vlen;
		int quoted;

		while (rec_spec_scan(&cur, &key, &klen, &val, &vlen,
				     &quoted)) {
			if (rec_key_eq(key, klen, "field")) {
				f_leaf = val;
				flen = vlen;
				f_quoted = quoted;
				has_field = 1;
			} else if (rec_key_eq(key, klen, "query")) {
				q_leaf = val;
				qlen = vlen;
				q_quoted = quoted;
				has_query = 1;
			} else if (rec_key_eq(key, klen, "phrase")) {
				if (rec_cli_int_b(val, val + vlen,
						  &p->phrase) == 0)
					has_phrase = 1;
			} else if (rec_key_eq(key, klen, "matched")) {
				if (rec_cli_size_b(val, val + vlen,
						   &p->matched) == 0)
					has_matched = 1;
			}
		}
	}
	/* D14 CLI merge (round 2): fill the fields the leaf omitted. Leaf
	 * wins — even an empty leaf value (field= → the "text" default,
	 * exactly as before). */
	if (has_query) {
		if (rec_cli_str_dup(q_leaf, qlen, q_quoted,
				    &q_owned) != 0)
			goto fail;
		p->query = q_owned;
	} else if (stoma_cli_query) {
		p->query = stoma_cli_query;
	}
	if (has_field) {
		if (flen > 0) {
			if (rec_cli_str_dup(f_leaf, flen, f_quoted,
					    &f_owned) != 0)
				goto fail;
			p->field = f_owned;
		}                       /* empty leaf → keep "text" default */
	} else if (stoma_cli_field_set) {
		p->field = stoma_cli_field;
	}
	if (!has_phrase && stoma_cli_phrase_set)
		p->phrase = stoma_cli_phrase;
	if (!has_matched && stoma_cli_matched_set)
		p->matched = stoma_cli_matched;
	return p;

fail:
	free(q_owned);
	free(f_owned);
	free(p);
	return NULL;
}

__attribute__((constructor)) static void stoma_rec_axis_init(void)
{
	static const rec_axis_t stoma_axis = {
		"stoma", stoma_axis_fill, stoma_axis_rank, NULL, stoma_axis_decode
	};

	rec_axis_register(&stoma_axis);
}

/*
 * rec_axis_open convention (RECALL-KERNEL.md "rec_axis_open convention", optional CLI-open
 * convention, not part of libcorm's core rec_query registry API): spec
 * is the decimal corm hash mask for stoma_open() (empty/NULL -> 0, the
 * corm default) — or, when it names a path (contains a '/'), a
 * 2B-2 primary-seeded rebuild: stoma finds the primary corm store and
 * re-indexes every stored record (`:a:s` raw and `:a:u` decimal alike —
 * keys iterate as refs, values as text), emitting the one-line rebuild
 * budget note (mm-plan U4).
 *
 * Primary resolution (7-AXIS-NAMESPACE-PLAN.md B-1/B-2): the CLI publishes
 * the exact primary it opened as CORM_AXIS_PRIMARY, and stoma rebuilds
 * from THAT primary (verbatim → aliases the live handle) when its
 * `<primary>.roster` sidecar exists. Without the env it falls back to the
 * directory scan (exactly one `*.roster` → use it; none → loud warn, empty).
 * It NEVER guesses between sibling DBs: two or more rosters without the
 * env → loud warn, memory-only index (never silently empty, never the
 * wrong primary's data).
 *
 * Malformed/unfindable sidecar → loud warn, memory-only index. The
 * primary open uses the CLI's mask derivation (D11): CORM_MASK env
 * (validated 2^n-1) else the 4095 default — co-opened files must always
 * match. Returns the stoma_db_t* ctx directly (no cast needed).
 */
void *rec_axis_open(const char *spec)
{
	unsigned mask;

	if (spec && strchr(spec, '/')) {
		stoma_db_t *db = stoma_open(0);
		/* CLI mask derivation (D11): CORM_MASK overrides the 4095
		 * default — the sidecar rebuild must open the primary with
		 * the same table shape the CLI wrote. */
		const char *menv = getenv("CORM_MASK");
		const char *eprim = getenv("CORM_AXIS_PRIMARY");
		unsigned pmask = 4096 - 1;
		char *tmp = strdup(spec);
		char *dir;
		char *primary = NULL;
		DIR *d;
		struct dirent *de;
		uint32_t hd;
		uint32_t cur;
		const void *key, *value;
		size_t docs = 0;
		struct timespec t0, t1;
		double ms;
		/* -1 = directory scan never ran (CORM_AXIS_PRIMARY used), else
		 * the count of *.roster sidecars the scan saw. */
		int nroster = -1;

		if (menv && *menv) {
			unsigned long v = strtoul(menv, NULL, 10);
			if (v != 0 && (v & (v + 1)) == 0)
				pmask = (unsigned) v;
		}

		if (!db) {
			free(tmp);
			return NULL;
		}
		if (tmp)
			dir = dirname(tmp);
		else
			dir = (char *)spec;

		/* B-1 (7-AXIS-NAMESPACE-PLAN.md): the CLI published the exact
		 * primary it opened. Rebuild from that verbatim path when its
		 * roster sidecar exists — it aliases the live handle and can
		 * never mis-pick a sibling DB. */
		if (eprim && *eprim) {
			char rs[BUFSIZ];
			struct stat st;

			snprintf(rs, sizeof(rs), "%s.roster", eprim);
			if (stat(rs, &st) == 0)
				primary = strdup(eprim);
			else
				fprintf(stderr,
					"stoma: CORM_AXIS_PRIMARY '%s' has no "
					"'%s'; falling back to the directory "
					"scan\n",
					eprim, rs);
		}

		if (!primary) {
			nroster = 0;

			d = opendir(dir);
			if (d) {
				while ((de = readdir(d))) {
					static const char suf[] = ".roster";
					size_t n = strlen(de->d_name);
					size_t sl = sizeof(suf) - 1;

					if (n <= sl ||
							strcmp(de->d_name + n - sl,
								suf) != 0)
						continue;
					nroster++;
					if (nroster == 1) {
						size_t need = strlen(dir) + 1
							+ (n - sl) + 1;
						primary = malloc(need);
						if (primary)
							snprintf(primary, need,
								"%s/%.*s", dir,
								(int)(n - sl),
								de->d_name);
					}
				}
				closedir(d);
			}

			/* B-2: never guess between sibling DBs. Zero rosters =
			 * no primary (existing warn); two+ without the env =
			 * loud warn, stay empty (not the wrong primary). */
			if (nroster > 1) {
				fprintf(stderr,
					"stoma: %d roster sidecars in '%s'; "
					"cannot pick a primary (set "
					"CORM_AXIS_PRIMARY to one DB); text "
					"queries stay empty\n",
					nroster, dir);
				free(primary);
				primary = NULL;
			}
		}

		if (!primary && nroster == 0) {
			fprintf(stderr,
				"stoma: no primary found in '%s' (no *.roster "
				"sidecar); text queries stay empty\n",
				dir);
		} else {
			hd = corm_open(primary, "hd", CM_HNDL, CM_STR,
					pmask, CM_AINDEX | CM_MIRROR);
			if (!hd) {
				fprintf(stderr,
					"stoma: cannot open primary '%s'\n",
					primary);
			} else {
				clock_gettime(CLOCK_MONOTONIC, &t0);
				cur = corm_iter(hd, NULL, 0);
				while (corm_next(&key, &value, cur)) {
					rec_ref_t ref = 0;
					memcpy(&ref, key,
							corm_type_len(CM_HNDL));
					if (value)
						stoma_index_ref(db,
							STOMA_AXIS_TEXT_FIELD,
							ref, (const char *)value);
					docs++;
				}
				corm_fin(cur);
				clock_gettime(CLOCK_MONOTONIC, &t1);
				ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0
					+ (double)(t1.tv_nsec - t0.tv_nsec)
					/ 1000000.0;
				fprintf(stderr, "stoma: rebuilt %zu docs in "
						"%.2f ms\n", docs, ms);
			}
		}
		free(primary);
		free(tmp);
		return db;
	}

	mask = (spec && *spec) ? (unsigned)strtoul(spec, NULL, 10) : 0;
	return stoma_open(mask);
}

/*
 * rec_axis_cli_options / rec_axis_config_arg conventions (D14, round 2:
 * optional axis-contributed CLI options — not libcorm core API): corm
 * collects inline `--name=value` tokens and, once every bound axis is
 * connected, broadcasts each to the declarations via these dlsym'd symbols.
 * stoma declares `query` (full-text query), `field` (query field), `phrase`
 * and `matched`; decode merges them into the bare/empty/leaf paths (leaf
 * spec wins; field defaults to "text"). The option struct ABI is
 * kernel-owned in <ttypt/rec.h>.
 */
const struct rec_axis_cli_option *
rec_axis_cli_options(void)
{
	static const struct rec_axis_cli_option opts[] = {
		{ "query", 1, "full-text query" },
		{ "field", 1, "text field to query" },
		{ "phrase", 1, "phrase match 0|1" },
		{ "matched", 1, "rank matched-token numerator" },
		{ NULL, 0, NULL }
	};
	return opts;
}

int
rec_axis_config_arg(const char *name, const char *value)
{
	int iv;
	size_t nv;

	if (!name || !value)
		return -1;
	if (!strcmp(name, "query"))
		return rec_cli_str_set(&stoma_cli_query, value);
	if (!strcmp(name, "field")) {
		if (rec_cli_str_set(&stoma_cli_field, value) != 0)
			return -1;
		stoma_cli_field_set = 1;
		return 0;
	}
	if (!strcmp(name, "phrase")) {
		if (rec_cli_int(value, &iv) != 0 || (iv != 0 && iv != 1))
			return -1;
		stoma_cli_phrase = iv;
		stoma_cli_phrase_set = 1;
		return 0;
	}
	if (!strcmp(name, "matched")) {
		if (rec_cli_size(value, &nv) != 0)
			return -1;
		stoma_cli_matched = nv;
		stoma_cli_matched_set = 1;
		return 0;
	}
	return -1;
}
