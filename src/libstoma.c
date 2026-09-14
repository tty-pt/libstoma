#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <libgen.h>
#include <time.h>
#include <ttypt/qmap.h>
#include "stoma/stoma.h"

#define STOMA_MAX_TOKENS 64

/* Declared in token.c (internal). */
void stoma_tokenize(
        const char *folded, void (*cb)(const char *tok, size_t len, void *user),
        void *user);

struct stoma_db {
	unsigned hd;     /* QM_SORTED, QM_STR → QM_STR inverted index */
	unsigned doc_hd; /* QM_SORTED, QM_STR → QM_STR folded text per
	                    (field,row) */
};

stoma_db_t *stoma_open(unsigned mask)
{
	stoma_db_t *db;

	db = calloc(1, sizeof(*db));
	if (!db)
		return NULL;
	db->hd = qmap_open(NULL, NULL, QM_STR, QM_STR, mask, QM_SORTED);
	if (!db->hd) {
		free(db);
		return NULL;
	}
	db->doc_hd = qmap_open(NULL, NULL, QM_STR, QM_STR, mask, QM_SORTED);
	if (!db->doc_hd) {
		qmap_close(db->hd);
		free(db);
		return NULL;
	}
	return db;
}

void stoma_close(stoma_db_t *db)
{
	if (!db)
		return;
	qmap_close(db->hd);
	qmap_close(db->doc_hd);
	free(db);
}

void stoma_clear(stoma_db_t *db)
{
	if (!db)
		return;
	qmap_drop(db->hd);
	qmap_drop(db->doc_hd);
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
	qmap_put(ctx->hd, key, "");
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
			qmap_put(db->doc_hd, dkey, fdoc);
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
	dtext = (const char *)qmap_get(db->doc_hd, dkey);
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
		qmap_del(db->hd, key);
		free(key);
	}
	free(toks.ent);
	qmap_del(db->doc_hd, dkey);
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
	dtext = (const char *)qmap_get(db->doc_hd, dkey);
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
		qmap_put(d->hd, row_id, "");
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
	size_t fld;
	size_t qlen;
	char *folded;
	unsigned cur_hd = 0;
	uint32_t matches = 0;
	size_t i;

	if (handled)
		*handled = 0;
	if (!db || !field || !query || !out)
		return 0;
	if (!out->hd && !out->set)
		return 0;
	/* The fold never grows its output, so strlen+1 always fits. */
	qlen = strlen(query);
	folded = malloc(qlen + 1);
	if (!folded)
		return 0;
	if (stoma_fold(folded, qlen + 1, query) < 0) {
		free(folded);
		return 0;
	}

	toks.n = 0;
	stoma_tokenize(folded, collect_token, &toks);
	if (toks.n == 0) {
		free(folded);
		return 0;
	}
	if (handled)
		*handled = 1;

	fld = strlen(field);

	for (i = 0; i < toks.n; i++) {
		unsigned nxt_hd;
		size_t plen = fld + 1 + toks.len[i];
		char *prefix = malloc(plen + 1);
		uint32_t cur;
		const void *k;
		const void *v;

		if (!prefix)
			break;
		memcpy(prefix, field, fld);
		prefix[fld] = '\t';
		memcpy(prefix + fld + 1, toks.toks[i], toks.len[i]);
		prefix[plen] = '\0';

		nxt_hd = qmap_open(NULL, NULL, QM_STR, QM_STR, 0xFF, 0);
		if (!nxt_hd) {
			free(prefix);
			break;
		}

		/* Prefix scan: index map is QM_SORTED, QM_RANGE iterates from
		 * the lower bound to the end; break once the prefix no longer
		 * matches (contiguous keys). */
		cur = qmap_iter(db->hd, prefix, QM_RANGE);
		while (qmap_next(&k, &v, cur)) {
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
			if (!cur_hd || qmap_get(cur_hd, sep2 + 1))
				qmap_put(nxt_hd, sep2 + 1, "");
		}
		qmap_fin(cur);
		free(prefix);

		if (cur_hd)
			qmap_close(cur_hd);
		cur_hd = nxt_hd;
	}

	if (cur_hd) {
		uint32_t cur = qmap_iter(cur_hd, NULL, 0);
		const void *k;
		const void *v;

		while (qmap_next(&k, &v, cur)) {
			const char *rid = (const char *)k;
			int keep = 1;

			if (phrase && toks.n > 1) {
				size_t dfl = fld + 1 + strlen(rid);
				char *dkey = malloc(dfl + 1);
				const char *dtext;
				dctoks_t d;

				if (!dkey)
					break;
				memcpy(dkey, field, fld);
				dkey[fld] = '\t';
				memcpy(dkey + fld + 1, rid, strlen(rid));
				dkey[dfl] = '\0';
				dtext = (const char *)qmap_get(
				        db->doc_hd, dkey);
				free(dkey);
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
		qmap_fin(cur);
		qmap_close(cur_hd);
	}

	free(folded);

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
	dtext = (const char *)qmap_get(ctx->db->doc_hd, dkey);
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
 * heap-owned rec_stoma_params (freed never — one-shot CLI process lifetime,
 * matches the other axis decode fns). query/field default to "", phrase/
 * matched default to 0. Single-quoted values may contain spaces.
 */
static void *stoma_axis_decode(const char *s)
{
	struct rec_stoma_params *p;
	char *buf, *cur;

	if (!s)
		return NULL;
	p = calloc(1, sizeof(*p));
	buf = malloc(strlen(s) + 1);
	if (!p || !buf) {
		free(p);
		free(buf);
		return NULL;
	}
	strcpy(buf, s);
	p->field = "";
	p->query = "";
	p->phrase = 0;
	p->matched = 0;
	cur = buf;
	while (*cur) {
		char *key, *val;
		size_t vlen;

		while (*cur == ' ')
			cur++;
		if (!*cur)
			break;
		key = cur;
		while (*cur && *cur != '=' && *cur != ' ')
			cur++;
		if (*cur != '=') {
			if (*cur)
				cur++;
			continue;
		}
		*cur++ = '\0';
		if (*cur == '\'') {
			cur++;
			val = cur;
			while (*cur && *cur != '\'')
				cur++;
			vlen = (size_t)(cur - val);
			if (*cur == '\'')
				*cur++ = '\0';
		} else {
			val = cur;
			while (*cur && *cur != ' ')
				cur++;
			vlen = (size_t)(cur - val);
			if (*cur)
				*cur++ = '\0';
		}
		(void)vlen;
		if (!strcmp(key, "field"))
			p->field = val;
		else if (!strcmp(key, "query"))
			p->query = val;
		else if (!strcmp(key, "phrase"))
			p->phrase = atoi(val);
		else if (!strcmp(key, "matched"))
			p->matched = (size_t)atol(val);
	}
	return p;
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
 * convention, not part of libqmap's core rec_query registry API): spec
 * is the decimal qmap hash mask for stoma_open() (empty/NULL -> 0, the
 * qmap default) — or, when it names a path (contains a '/'), a
 * 2B-2 primary-seeded rebuild: stoma finds the primary qmap store via the
 * CLI's `<primary>.roster` sidecar inside spec's directory, re-indexes
 * every stored record (`:a:s` raw and `:a:u` decimal alike — keys
 * iterate as refs, values as text), and emits the one-line rebuild
 * budget note (mm-plan U4). Malformed/unfindable sidecar → loud warn,
 * memory-only index (never silently empty). The primary open uses the
 * CLI's mask derivation (D11): QMAP_MASK env (validated 2^n-1) else the
 * 4095 default — co-opened files must always match. Returns the
 * stoma_db_t* ctx directly (no cast needed).
 */
void *rec_axis_open(const char *spec)
{
	unsigned mask;

	if (spec && strchr(spec, '/')) {
		stoma_db_t *db = stoma_open(0);
		/* CLI mask derivation (D11): QMAP_MASK overrides the 4095
		 * default — the sidecar rebuild must open the primary with
		 * the same table shape the CLI wrote. */
		const char *menv = getenv("QMAP_MASK");
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

		/* Primary = the <base> a `<base>.roster` sidecar prefixes. */
		d = opendir(dir);
		if (d) {
			while ((de = readdir(d))) {
				static const char suf[] = ".roster";
				size_t n = strlen(de->d_name);
				size_t sl = sizeof(suf) - 1;

				if (n <= sl ||
						strcmp(de->d_name + n - sl, suf) != 0)
					continue;
				{
					size_t need = strlen(dir) + 1
						+ (n - sl) + 1;
					primary = malloc(need);
					if (primary)
						snprintf(primary, need, "%s/%.*s",
								dir, (int)(n - sl),
								de->d_name);
				}
				break;
			}
			closedir(d);
		}

		if (!primary) {
			fprintf(stderr,
				"stoma: no primary found in '%s' (no *.roster "
				"sidecar); text queries stay empty\n",
				dir);
		} else {
			hd = qmap_open(primary, "hd", QM_HNDL, QM_STR,
					pmask, QM_AINDEX | QM_MIRROR);
			if (!hd) {
				fprintf(stderr,
					"stoma: cannot open primary '%s'\n",
					primary);
			} else {
				clock_gettime(CLOCK_MONOTONIC, &t0);
				cur = qmap_iter(hd, NULL, 0);
				while (qmap_next(&key, &value, cur)) {
					rec_ref_t ref = 0;
					memcpy(&ref, key,
							qmap_type_len(QM_HNDL));
					if (value)
						stoma_index_ref(db,
							STOMA_AXIS_TEXT_FIELD,
							ref, (const char *)value);
					docs++;
				}
				qmap_fin(cur);
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
