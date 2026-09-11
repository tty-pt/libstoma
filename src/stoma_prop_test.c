/*
 * stoma_prop_test — differential (randomized) test for stoma.
 *
 * Generates deterministic random rows and queries (fixed LCG seeds), runs
 * stoma_query for each, and compares the result against an independent
 * reference implementation built solely on the public API (stoma_fold + a
 * local tokenizer + per-token prefix AND). Any mismatch is reported with the
 * seed and inputs, and the run fails. Seeds are fixed so failures reproduce.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <ttypt/qmap.h>
#include <ttypt/rec.h>
#include "stoma/stoma.h"

#define MAX_ROWS 300
#define MAX_FIELDS 2
#define MAX_QT 3
#define MAX_VT 64
#define MAX_QUERIES 1500
#define TOK_CAP 31

static const char *field_names[MAX_FIELDS] = { "title", "author" };

static const char *vocab[] = {
	"starlight", "station",   "nostalgia",  "black",   "star",
	"night",     "dark",      "love",       "world",   "another",
	"song",      "1984",      "yesterday",  "coffee",  "river",
	"mountain",  "summer",    "winter",     "spring",  "autumn",
	"café",      "naïve",     "Übermensch", "El Niño", "Straße",
	"Ærø",       "Ångström",  "cœur",       "crème",   "über",
	"日本語",    "sky",       "moon",       "sea",     "fire",
	"coldplay",  "radiohead", "neon",       "green",   "blue",
};
#define NVOCAB ((int)(sizeof(vocab) / sizeof(vocab[0])))

static const char *punct[] = { "", "...", ",", "!!!", "   ", "-", "?" };
#define NPUNCT ((int)(sizeof(punct) / sizeof(punct[0])))

/* Fixed LCG — deterministic, no libc rand() */
static unsigned long rng_state;

static unsigned rnd(void)
{
	rng_state = rng_state * 1103515245UL + 12345UL;
	return (unsigned)((rng_state >> 16) & 0x7FFF);
}

static unsigned pick(unsigned n)
{
	return rnd() % n;
}

typedef struct {
	char tok[MAX_VT][TOK_CAP + 1];
	int n;
} toks_t;

/* Fold + tokenize a value exactly the way stoma does, but independently. */
static void fold_tokens(const char *value, toks_t *t)
{
	char folded[8192];
	const char *p;
	int n = 0;

	t->n = 0;
	if (!value)
		return;
	if (stoma_fold(folded, sizeof(folded), value) < 0)
		return;
	p = folded;
	while (*p) {
		const char *start;
		int len;

		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		      (unsigned char)*p >= 0x80))
		{
			p++;
			continue;
		}
		start = p;
		len = 0;
		while (*p &&
		       ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		        (unsigned char)*p >= 0x80))
		{
			p++;
			len++;
		}
		if (n < MAX_VT) {
			if (len > TOK_CAP)
				len = TOK_CAP;
			memcpy(t->tok[n], start, (size_t)len);
			t->tok[n][len] = '\0';
			n++;
		}
	}
	t->n = n;
}

static toks_t row_toks[MAX_ROWS][MAX_FIELDS];
static int row_has[MAX_ROWS][MAX_FIELDS];
static stoma_db_t *db;

static void gen_row_value(char *value, size_t vsz, int *present, toks_t *t)
{
	int nt = (int)pick((unsigned)(MAX_VT + 1));
	int off = 0;
	int i;

	*present = (int)pick(4) != 0;
	if (!*present) {
		t->n = 0;
		value[0] = '\0';
		return;
	}
	if (nt == 0) {
		const char *p = punct[pick(NPUNCT)];
		size_t l = strlen(p);

		if (l >= vsz)
			l = vsz - 1;
		memcpy(value, p, l);
		value[l] = '\0';
	} else {
		for (i = 0; i < nt; i++) {
			const char *w = vocab[pick(NVOCAB)];
			int l = (int)strlen(w);

			if (off + l + 2 >= (int)vsz)
				break;
			memcpy(value + off, w, (size_t)l);
			off += l;
			value[off++] = ' ';
		}
		if (off > 0)
			value[--off] = '\0';
	}
	fold_tokens(value, t);
}

static void gen_query(char *query, size_t qsz)
{
	int nt = 1 + (int)pick(MAX_QT);
	int off = 0;
	int i;

	if (pick(10) == 0) {
		const char *p = punct[pick(NPUNCT)];
		size_t l = strlen(p);

		if (l >= qsz)
			l = qsz - 1;
		memcpy(query, p, l);
		query[l] = '\0';
		return;
	}
	for (i = 0; i < nt; i++) {
		const char *w = vocab[pick(NVOCAB)];
		char tmp[TOK_CAP + 1];
		int len = (int)strlen(w);
		int cut;
		int random = 0;
		int j;

		switch (pick(4)) {
		case 0:
			cut = 1;
			break;
		case 1:
			cut = 2 + (int)pick(3);
			break;
		case 2:
			cut = len;
			break;
		default:
			/* random string (may or may not match) */
			cut = 2 + (int)pick(3);
			random = 1;
			break;
		}
		if (cut > len)
			cut = len;
		if (cut < 0)
			cut = 0;
		if (random) {
			for (j = 0; j < cut; j++)
				tmp[j] = (char)('a' + pick(26));
			tmp[cut] = '\0';
		} else {
			memcpy(tmp, w, (size_t)cut);
			tmp[cut] = '\0';
			/* occasional uppercase variant */
			if (cut > 0 && pick(3) == 0)
				for (j = 0; j < cut; j++)
					if (tmp[j] >= 'a' && tmp[j] <= 'z')
						tmp[j] = (char)(tmp[j] - 32);
		}
		if (off + cut + 2 >= (int)qsz)
			break;
		memcpy(query + off, tmp, (size_t)cut);
		off += cut;
		query[off++] = ' ';
	}
	if (off > 0)
		query[--off] = '\0';
	else
		query[0] = '\0';
}

static int ref_matches(int row, int fi, const toks_t *qt)
{
	int i;

	if (qt->n == 0)
		return 0;
	for (i = 0; i < qt->n; i++) {
		int t;
		int ok = 0;

		for (t = 0; t < row_toks[row][fi].n; t++)
			if (strncmp(row_toks[row][fi].tok[t], qt->tok[i],
			            strlen(qt->tok[i])) == 0)
			{
				ok = 1;
				break;
			}
		if (!ok)
			return 0;
	}
	return 1;
}

/* Phrase reference: the query tokens must occur as a contiguous subsequence
 * of the row's token stream, with per-token prefix matching. */
static int ref_phrase_matches(int row, int fi, const toks_t *qt)
{
	int i;
	int j;

	if (qt->n == 0)
		return 0;
	if (qt->n > row_toks[row][fi].n)
		return 0;
	for (i = 0; i + qt->n <= row_toks[row][fi].n; i++) {
		for (j = 0; j < qt->n; j++)
			if (strncmp(row_toks[row][fi].tok[i + j], qt->tok[j],
			            strlen(qt->tok[j])) != 0)
				break;
		if (j == qt->n)
			return 1;
	}
	return 0;
}

/* Differential check of rec_axis_fill_tokens against the independent
 * reference (match_fn) plus set invariants and the exact rank formula for
 * single-token queries. Returns the failure count for this query. */
static int check_fill(unsigned long seed, int q, const char *query, int fi,
                      const toks_t *qt, int phrase,
                      int (*match_fn)(int, int, const toks_t *))
{
	rec_set_t *fs = rec_set_new();
	const rec_ref_t *s;
	size_t cnt;
	int rc;
	int bad = 0;
	int r;
	int exp_total = 0;

	rc = rec_axis_fill_tokens(db, field_names[fi], query, phrase, fs);
	cnt = rec_set_count(fs);
	s = rec_set_at(fs);
	if (rc != 0) {
		printf("seed %lu q%d: fill rc=%d query='%s'\n", seed, q, rc,
		       query);
		bad = 1;
		goto out;
	}
	if (qt->n == 0 && cnt != 0) {
		printf("seed %lu q%d: fill zero-token mismatch (cnt=%zu) "
		       "query='%s'\n",
		       seed, q, cnt, query);
		bad = 1;
		goto out;
	}
	for (r = 0; r < MAX_ROWS; r++)
		if (match_fn(r, fi, qt)) {
			size_t lo = 0, hi = cnt, mid;

			exp_total++;
			while (lo < hi) {
				mid = lo + (hi - lo) / 2;
				if (s[mid] < (rec_ref_t)r)
					lo = mid + 1;
				else
					hi = mid;
			}
			if (lo >= cnt || s[lo] != (rec_ref_t)r) {
				printf("seed %lu q%d: fill missing ref %d "
				       "field '%s' query='%s'\n",
				       seed, q, r, field_names[fi], query);
				bad++;
			}
		}
	if ((size_t)exp_total != cnt) {
		printf("seed %lu q%d: fill count mismatch (fill=%zu "
		       "ref=%d)\n",
		       seed, q, cnt, exp_total);
		bad++;
	}
	for (r = 1; r < (int)cnt; r++)
		if (s[r] <= s[r - 1]) {
			printf("seed %lu q%d: fill set not sorted/unique\n",
			       seed, q);
			bad++;
			break;
		}
	/* exact score spot-check: single query token => matched = 1; skip
	 * cap-edge docs (>= MAX_VT tokens) and zero-token docs (rank -1). */
	if (qt->n == 1)
		for (r = 0; r < MAX_ROWS; r++)
			if (match_fn(r, fi, qt) && row_toks[r][fi].n > 0 &&
			    row_toks[r][fi].n < MAX_VT) {
				struct stoma_rank_ctx c = { db,
				                              field_names[fi],
				                              1 };
				float score;
				float e;
				float err;

				if (stoma_rank(&c, (rec_ref_t)r, &score) != 0 ||
				    score <= 0.0f) {
					printf("seed %lu q%d: rank fail row "
					       "%d\n",
					       seed, q, r);
					bad++;
					continue;
				}
				e = 1.0f / (float)row_toks[r][fi].n;
				err = score - e;
				if (err < 0.0f)
					err = -err;
				if (err > 1e-6f) {
					printf("seed %lu q%d: rank score row "
					       "%d score=%g exp=%g\n",
					       seed, q, r, (double)score,
					       (double)e);
					bad++;
				}
			}
out:
	rec_set_free(fs);
	return bad;
}

static int run_seed(unsigned long seed)
{
	int failures = 0;
	int q;

	rng_state = seed;
	stoma_clear(db);

	/* generate + index rows */
	{
		int r;
		int f;

		for (r = 0; r < MAX_ROWS; r++)
			for (f = 0; f < MAX_FIELDS; f++) {
				char value[256];
				char rid[16];

				gen_row_value(
				        value, sizeof(value), &row_has[r][f],
				        &row_toks[r][f]);
				if (!row_has[r][f])
					continue;
				snprintf(rid, sizeof(rid), "%d", r);
				if (stoma_index(
				            db, field_names[f], rid, value) !=
				    0)
					failures++;
			}
	}

	/* run queries, compare against reference */
	for (q = 0; q < MAX_QUERIES; q++) {
		char query[512];
		toks_t qt;
		int fi = (int)pick(MAX_FIELDS);
		unsigned out_hd;
		int handled = 0;
		uint32_t n;
		int exp_total = 0;
		int r;

		gen_query(query, sizeof(query));
		fold_tokens(query, &qt);

		out_hd = qmap_open(NULL, NULL, QM_STR, QM_STR, 0xFF, 0);
		n = stoma_query(db, field_names[fi], query, out_hd, &handled);

		if ((qt.n > 0) != (handled > 0)) {
			printf("seed %lu q%d: handled mismatch "
			       "(stoma=%d ref=%d) query='%s'\n",
			       seed, q, handled, qt.n > 0, query);
			failures++;
			qmap_close(out_hd);
			continue;
		}
		for (r = 0; r < MAX_ROWS; r++) {
			char rid[16];
			int exp = handled && ref_matches(r, fi, &qt);
			int got;

			snprintf(rid, sizeof(rid), "%d", r);
			got = qmap_get(out_hd, rid) != NULL;
			exp_total += exp;
			if (got != exp) {
				if (failures < 5)
					printf("seed %lu q%d: row %s "
					       "field '%s' query='%s' "
					       "stoma=%d ref=%d\n",
					       seed, q, rid, field_names[fi],
					       query, got, exp);
				failures++;
			}
		}
		if ((uint32_t)exp_total != n) {
			printf("seed %lu q%d: count mismatch "
			       "(stoma=%u ref=%d) query='%s'\n",
			       seed, q, (unsigned)n, exp_total, query);
			failures++;
		}
		qmap_close(out_hd);
		failures += check_fill(
		        seed, q, query, fi, &qt, 0, ref_matches);

		/* phrase variant: the same query through stoma_query_phrase,
		 * compared against the contiguous-subsequence reference. */
		{
			unsigned out_hd2 =
			        qmap_open(NULL, NULL, QM_STR, QM_STR, 0xFF, 0);
			uint32_t n2 = 0;
			int handled2 = 0;
			int exp_total2 = 0;
			int r2;

			n2 = stoma_query_phrase(
			        db, field_names[fi], query, out_hd2, &handled2);
			if ((qt.n > 0) != (handled2 > 0)) {
				printf("seed %lu q%d: phrase handled mismatch "
				       "(stoma=%d ref=%d) query='%s'\n",
				       seed, q, handled2, qt.n > 0, query);
				failures++;
				qmap_close(out_hd2);
				continue;
			}
			for (r2 = 0; r2 < MAX_ROWS; r2++) {
				char rid2[16];
				int exp2 = handled2 &&
				           ref_phrase_matches(r2, fi, &qt);
				int got2;

				snprintf(rid2, sizeof(rid2), "%d", r2);
				got2 = qmap_get(out_hd2, rid2) != NULL;
				exp_total2 += exp2;
				if (got2 != exp2) {
					if (failures < 5)
						printf("seed %lu q%d: phrase "
						       "row "
						       "%s field '%s' "
						       "query='%s' "
						       "stoma=%d ref=%d\n",
						       seed, q, rid2,
						       field_names[fi], query,
						       got2, exp2);
					failures++;
				}
			}
			if ((uint32_t)exp_total2 != n2) {
				printf("seed %lu q%d: phrase count mismatch "
				       "(stoma=%u ref=%d) query='%s'\n",
				       seed, q, (unsigned)n2, exp_total2,
				       query);
				failures++;
			}
			qmap_close(out_hd2);
			failures += check_fill(seed, q, query, fi, &qt, 1,
			                      ref_phrase_matches);
		}
	}

	printf("seed %lu: %d rows, %d queries%s\n", seed, MAX_ROWS, MAX_QUERIES,
	       failures ? " FAILED" : " OK");
	return failures;
}

int main(void)
{
	static const unsigned long seeds[] = { 1, 42, 1337 };
	int total_fail = 0;
	size_t i;

	db = stoma_open(0);
	if (!db) {
		printf("stoma_open failed\n");
		return 1;
	}

	for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
		total_fail += run_seed(seeds[i]);

	stoma_close(db);

	if (total_fail) {
		printf("Results: %d FAILURES\n", total_fail);
		return 1;
	}
	printf("Results: all seeds passed\n");
	return 0;
}
