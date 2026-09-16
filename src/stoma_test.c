#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <ttypt/qmap.h>
#include <ttypt/rec.h>
#include "stoma/stoma.h"

static int failures = 0;
static int total = 0;
static stoma_db_t *rdb;

#define CHECK(cond, name)                                                      \
	do {                                                                   \
		total++;                                                       \
		if (!(cond)) {                                                 \
			failures++;                                            \
			printf("FAIL: %s (line %d)\n", name, __LINE__);        \
		}                                                              \
	} while (0)

static int hd_has(unsigned hd, const char *row)
{
	return qmap_get(hd, row) != NULL;
}

/* ---- recall-kernel helpers (groups 30-35) ---- */

static int rec_has(const rec_set_t *fs, rec_ref_t r)
{
	const rec_ref_t *s = rec_set_at(fs);
	size_t n = rec_set_count(fs);
	size_t lo = 0, hi = n, mid;

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (s[mid] < r)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && s[lo] == r;
}

static int rec_sorted_unique(const rec_set_t *fs)
{
	const rec_ref_t *s = rec_set_at(fs);
	size_t n = rec_set_count(fs);
	size_t i;

	for (i = 1; i < n; i++)
		if (s[i] <= s[i - 1])
			return 0;
	return 1;
}

static int qmap_equals_set(unsigned hd, const rec_set_t *fs)
{
	uint32_t cur = qmap_iter(hd, NULL, 0);
	const void *k;
	const void *v;
	size_t qn = 0;
	int ok = 1;

	while (ok && qmap_next(&k, &v, cur)) {
		const char *key = (const char *)k;
		char *end;
		unsigned long long rv;

		if (key[0] < '0' || key[0] > '9') {
			ok = 0;
			break;
		}
		rv = strtoull(key, &end, 10);
		ok = end && *end == '\0' && rec_has(fs, (rec_ref_t)rv);
		if (ok)
			qn++;
	}
	qmap_fin(cur);
	return ok && qn == rec_set_count(fs);
}

static rec_set_t *fill_once(stoma_db_t *db, const char *field,
                            const char *query, int phrase)
{
	rec_set_t *fs = rec_set_new();

	if (!fs)
		return NULL;
	if (rec_axis_fill_tokens(db, field, query, phrase, fs) != 0) {
		rec_set_free(fs);
		return NULL;
	}
	return fs;
}

static int float_close(float a, float b)
{
	float d = a - b;

	return d < 0 ? -d < 1e-6f : d < 1e-6f;
}

static void check_row_fill(unsigned out, const char *query, int hexp,
                           const rec_ref_t *exp, size_t nex)
{
	int handled = 0;
	uint32_t n;
	rec_set_t *fs;
	char name[128];

	qmap_drop(out);
	n = stoma_query(rdb, "title", query, out, &handled);
	fs = fill_once(rdb, "title", query, 0);
	snprintf(name, sizeof(name), "fill==query '%s'", query);
	CHECK(fs && handled == hexp && n == nex && rec_set_count(fs) == nex &&
	              qmap_equals_set(out, fs) &&
	              rec_sorted_unique(fs),
	      name);
	if (fs) {
		const rec_ref_t *s = rec_set_at(fs);
		size_t i;

		for (i = 0; i < nex; i++)
			CHECK(s[i] == exp[i], "fill member order");
		rec_set_free(fs);
	}
}

int main(void)
{
	stoma_db_t *db = stoma_open(0);
	unsigned out = qmap_open(NULL, NULL, QM_STR, QM_STR, 0xFF, 0);
	int handled = 0;
	uint32_t n;

	if (!db || !out) {
		printf("setup failed\n");
		return 1;
	}

	/* 1. fold */
	{
		char b[64];
		int r = stoma_fold(b, sizeof(b), "Été Aéro");
		CHECK(r > 0 && strcmp(b, "été aéro") == 0, "fold accent+case");
	}

	/* 2-3. index + exact + prefix */
	stoma_index(db, "title", "r1", "Starlight of the night");
	stoma_index(db, "title", "r2", "Station one");
	stoma_index(db, "title", "r3", "st");
	stoma_index(db, "title", "r4", "A Dark Night");

	n = stoma_query(db, "title", "night", out, &handled);
	CHECK(handled == 1 && n == 2, "exact token");
	CHECK(hd_has(out, "r1") && hd_has(out, "r4"), "exact rows");
	qmap_drop(out);

	n = stoma_query(db, "title", "st", out, &handled);
	CHECK(handled == 1 && n == 3, "prefix st");
	CHECK(hd_has(out, "r1") && hd_has(out, "r2") && hd_has(out, "r3"),
	      "prefix rows");
	CHECK(!hd_has(out, "r4"), "prefix excludes non-match");
	qmap_drop(out);

	/* 4. multi-token AND (order-insensitive) */
	stoma_index(db, "title", "r5", "Black Star");
	n = stoma_query(db, "title", "black star", out, &handled);
	CHECK(handled == 1 && n == 1 && hd_has(out, "r5"), "AND two tokens");
	qmap_drop(out);
	n = stoma_query(db, "title", "star black", out, &handled);
	CHECK(n == 1 && hd_has(out, "r5"), "AND order-insensitive");
	qmap_drop(out);

	/* 5. digits-only token */
	stoma_index(db, "title", "r6", "1984");
	n = stoma_query(db, "title", "1984", out, &handled);
	CHECK(handled == 1 && n == 1 && hd_has(out, "r6"), "digits token");
	qmap_drop(out);
	n = stoma_query(db, "title", "19", out, &handled);
	CHECK(n == 1 && hd_has(out, "r6"), "digit prefix");
	qmap_drop(out);

	/* 6. zero-token query → no-op */
	n = stoma_query(db, "title", "---", out, &handled);
	CHECK(handled == 0 && n == 0, "zero-token no-op");
	qmap_drop(out);

	/* 7. per-field isolation */
	stoma_index(db, "author", "r1", "Stardust");
	n = stoma_query(db, "title", "stardust", out, &handled);
	CHECK(handled == 1 && n == 0, "field isolation");
	qmap_drop(out);

	/* 8. token dedup: repeated word matches once */
	stoma_index(db, "title", "r7", "la la la");
	n = stoma_query(db, "title", "la", out, &handled);
	CHECK(handled == 1 && n == 1 && hd_has(out, "r7"), "dedup");
	qmap_drop(out);

	/* 9. clear + re-index */
	stoma_clear(db);
	n = stoma_query(db, "title", "night", out, &handled);
	CHECK(handled == 1 && n == 0, "clear removes entries");
	qmap_drop(out);
	stoma_index(db, "title", "r9", "new world");
	n = stoma_query(db, "title", "new", out, &handled);
	CHECK(n == 1 && hd_has(out, "r9"), "re-index after clear");
	qmap_drop(out);

	/* 10. in-place put on same (field,row): re-indexing a changed value
	 * leaves stale tokens (documented) — full rebuild via clear() is the
	 * caller's contract. */
	stoma_index(db, "title", "r9", "another world");
	n = stoma_query(db, "title", "new", out, &handled);
	CHECK(n == 1, "stale token remains (documented)");
	qmap_drop(out);

	/* 11. fold: lowercase, accents preserved (accent-sensitive) */
	{
		char b[128];

		CHECK(stoma_fold(b, sizeof(b), "Straße") > 0 &&
		              strcmp(b, "straße") == 0,
		      "fold sz");
		CHECK(stoma_fold(b, sizeof(b), "Øresund ærø å") > 0 &&
		              strcmp(b, "øresund ærø å") == 0,
		      "fold oslash ae aa");
		CHECK(stoma_fold(b, sizeof(b), "El Niño açúcar") > 0 &&
		              strcmp(b, "el niño açúcar") == 0,
		      "fold ntilde ccedil");
		CHECK(stoma_fold(b, sizeof(b), "Ünter Öl Ärger") > 0 &&
		              strcmp(b, "ünter öl ärger") == 0,
		      "fold umlauts");
		CHECK(stoma_fold(b, sizeof(b), "ÉTÉ") > 0 &&
		              strcmp(b, "été") == 0,
		      "fold case");
		CHECK(stoma_fold(b, sizeof(b), "Pão") > 0 &&
		              strcmp(b, "pão") == 0,
		      "fold pao preserves accent");
		CHECK(stoma_fold(b, sizeof(b), "não senhôr çãõ") > 0 &&
		              strcmp(b, "não senhôr çãõ") == 0,
		      "fold pt accents");
		CHECK(stoma_fold(b, sizeof(b), "À É Í Ó Ú à é í ó ú") > 0 &&
		              strcmp(b, "à é í ó ú à é í ó ú") == 0,
		      "fold upper accents");
	}

	/* 12. fold buffer boundaries */
	{
		char b[4];

		/* "abc" folds to 3 bytes + NUL → 4-byte buffer is an exact fit
		 */
		CHECK(stoma_fold(b, sizeof(b), "abc") == 3 &&
		              strcmp(b, "abc") == 0,
		      "fold exact fit");
		/* "abcd" = 4 bytes + NUL → 4-byte buffer too small → -1 */
		CHECK(stoma_fold(b, sizeof(b), "abcd") == -1,
		      "fold buffer too small");
		CHECK(stoma_fold(b, sizeof(b), "") == 0, "fold empty string");
	}

	/* 13. non-Latin text is preserved and searchable (accent-sensitive) */
	{
		char b[128];

		CHECK(stoma_fold(b, sizeof(b), "駅東京") == 9 &&
		              strcmp(b, "駅東京") == 0,
		      "fold cjk verbatim");
		stoma_index(db, "title", "r13", "駅東京");
		n = stoma_query(db, "title", "駅", out, &handled);
		CHECK(handled == 1 && n == 1, "cjk searchable");
		qmap_drop(out);
	}

	/* 14. NULL/empty argument contracts */
	CHECK(stoma_index(db, NULL, "r", "v") == -1, "index null field");
	CHECK(stoma_index(db, "t", NULL, "v") == -1, "index null row");
	CHECK(stoma_index(db, "t", "r", NULL) == -1, "index null value");
	CHECK(stoma_index(NULL, "t", "r", "v") == -1, "index null db");
	{
		int h = 99;

		n = stoma_query(NULL, "t", "q", out, &h);
		CHECK(n == 0 && h == 0, "query null db");
		n = stoma_query(db, NULL, "q", out, &h);
		CHECK(n == 0 && h == 0, "query null field");
		n = stoma_query(db, "t", NULL, out, &h);
		CHECK(n == 0 && h == 0, "query null query");
		n = stoma_query(db, "t", "q", 0, &h);
		CHECK(n == 0 && h == 0, "query null out_hd");
	}
	stoma_clear(NULL);
	stoma_close(NULL);

	/* 15. empty / punctuation-only values index nothing */
	stoma_index(db, "title", "r15", "");
	stoma_index(db, "title", "r16", "... !!! ---");
	n = stoma_query(db, "title", "anything", out, &handled);
	CHECK(handled == 1 && n == 0, "empty/punct values never match");
	qmap_drop(out);

	/* 16. out_hd is appended to, NOT cleared (caller contract) */
	stoma_index(db, "title", "r17", "alpha beta");
	stoma_index(db, "title", "r18", "gamma delta");
	qmap_drop(out);
	n = stoma_query(db, "title", "alpha", out, &handled);
	CHECK(n == 1 && hd_has(out, "r17"), "query alpha");
	n = stoma_query(db, "title", "gamma", out, &handled);
	CHECK(n == 1 && hd_has(out, "r17") && hd_has(out, "r18"),
	      "query appends (union)");
	qmap_drop(out);
	n = stoma_query(db, "title", "gamma", out, &handled);
	CHECK(n == 1 && !hd_has(out, "r17") && hd_has(out, "r18"),
	      "drop between queries isolates");

	/* 17. query truncates at STOMA_MAX_TOKENS (64) */
	{
		char big1[1024], big2[1024];
		size_t i;
		int off = 0;

		/* value has tokens t0..t64 + needle = 66 tokens (index is
		 * uncapped); query below has 64 non-matching tokens + needle
		 * as token #65 → #65 is dropped → no match. */
		for (i = 0; i < 65; i++)
			off += snprintf(
			        big1 + off, sizeof(big1) - (size_t)off, "t%zu ",
			        i);
		snprintf(big1 + off, sizeof(big1) - (size_t)off, "needle");
		stoma_index(db, "title", "r19", big1);
		qmap_drop(out);
		off = 0;
		for (i = 0; i < 64; i++)
			off += snprintf(
			        big2 + off, sizeof(big2) - (size_t)off,
			        "zz%zu ", i);
		snprintf(big2 + off, sizeof(big2) - (size_t)off, "needle");
		n = stoma_query(db, "title", big2, out, &handled);
		CHECK(handled == 1 && n == 0, "token 65+ ignored in query");
		qmap_drop(out);
		n = stoma_query(db, "title", "t0 t1 t2", out, &handled);
		CHECK(n == 1 && hd_has(out, "r19"), "first tokens still match");
		qmap_drop(out);
	}

	/* 17b. value larger than the old 8KB fold buffer (regression) */
	{
		/* 8980 'a's + " quarantinemon" (14 chars) = 8994 > 8192 */
		char *big = malloc(9000);
		size_t i;
		int off = 0;

		for (i = 0; i < 8980; i++)
			off += snprintf(big + off, 9000 - (size_t)off, "a");
		snprintf(big + off, 9000 - (size_t)off, " quarantinemon");
		stoma_index(db, "title", "r19b", big);
		free(big);
		qmap_drop(out);
		n = stoma_query(db, "title", "quarantinemon", out, &handled);
		CHECK(handled == 1 && n == 1 && hd_has(out, "r19b"),
		      "token past 8KB matches");
		qmap_drop(out);
		n = stoma_query(db, "title", "aaaa", out, &handled);
		CHECK(n == 1 && hd_has(out, "r19b"),
		      "early tokens still match");
		qmap_drop(out);
	}

	/* 18. query normalization: punctuation and extra spaces */
	stoma_index(db, "title", "r20", "Black Star");
	n = stoma_query(db, "title", "black,  star", out, &handled);
	CHECK(handled == 1 && n == 1 && hd_has(out, "r20"), "punct+spaces AND");
	qmap_drop(out);
	n = stoma_query(db, "title", "  BLACK   STAR ", out, &handled);
	CHECK(handled == 1 && n == 1 && hd_has(out, "r20"), "case+spaces AND");
	qmap_drop(out);

	/* 19. same token in two fields of one row stays field-isolated */
	stoma_index(db, "title", "r21", "love");
	stoma_index(db, "author", "r21", "love");
	stoma_index(db, "author", "r22", "uniquebyname");
	n = stoma_query(db, "title", "love", out, &handled);
	CHECK(handled == 1 && n == 1 && hd_has(out, "r21"), "title love");
	qmap_drop(out);
	n = stoma_query(db, "author", "love", out, &handled);
	CHECK(n == 1 && hd_has(out, "r21"), "author love");
	qmap_drop(out);
	n = stoma_query(db, "title", "uniquebyname", out, &handled);
	CHECK(n == 0, "no cross-field leak");
	qmap_drop(out);

	/* 20. alphanumeric tokens, prefix at token start only */
	stoma_index(db, "title", "r23", "song2 live");
	n = stoma_query(db, "title", "so", out, &handled);
	CHECK(handled == 1 && n == 1 && hd_has(out, "r23"), "alpha prefix");
	qmap_drop(out);
	n = stoma_query(db, "title", "song2", out, &handled);
	CHECK(n == 1 && hd_has(out, "r23"), "exact alnum token");
	qmap_drop(out);
	n = stoma_query(db, "title", "2l", out, &handled);
	CHECK(n == 0, "mid-token not matched");
	qmap_drop(out);

	/* 21. single-char query token */
	n = stoma_query(db, "title", "s", out, &handled);
	CHECK(handled == 1 && n == 2 && hd_has(out, "r20") &&
	              hd_has(out, "r23"),
	      "single-char token");
	qmap_drop(out);

	/* 22. phrase queries (stoma_query_phrase). Indexed tokens must avoid
	 * 's' initials — test 21 pins the 's' prefix count to r20+r23. */
	stoma_index(db, "title", "r24", "Blue Dawn");
	n = stoma_query_phrase(db, "title", "blue dawn", out, &handled);
	CHECK(handled == 1 && n == 1 && hd_has(out, "r24"),
	      "phrase adjacent in order");
	qmap_drop(out);
	n = stoma_query_phrase(db, "title", "dawn blue", out, &handled);
	CHECK(n == 0, "phrase rejects reorder");
	qmap_drop(out);
	n = stoma_query(db, "title", "dawn blue", out, &handled);
	CHECK(n == 1 && hd_has(out, "r24"), "AND stays order-insensitive");
	qmap_drop(out);

	stoma_index(db, "title", "r25", "Blue summer of the dawn");
	n = stoma_query_phrase(db, "title", "blue dawn", out, &handled);
	CHECK(n == 1 && hd_has(out, "r24") && !hd_has(out, "r25"),
	      "phrase rejects spread tokens");
	qmap_drop(out);
	n = stoma_query(db, "title", "blue dawn", out, &handled);
	CHECK(n == 2 && hd_has(out, "r24") && hd_has(out, "r25"),
	      "AND still matches spread tokens");
	qmap_drop(out);

	stoma_index(db, "title", "r26", "blackstar manor");
	n = stoma_query_phrase(db, "title", "black manor", out, &handled);
	CHECK(n == 1 && hd_has(out, "r26"), "phrase allows per-token prefix");
	qmap_drop(out);
	n = stoma_query_phrase(db, "title", "star manor", out, &handled);
	CHECK(n == 0, "phrase prefix must align at positions");
	qmap_drop(out);

	stoma_index(db, "title", "r27", "line one\nline two");
	n = stoma_query_phrase(db, "title", "one line", out, &handled);
	CHECK(n == 1 && hd_has(out, "r27"),
	      "phrase spans line break (token separator)");
	qmap_drop(out);
	n = stoma_query_phrase(db, "title", "two line", out, &handled);
	CHECK(n == 0, "phrase order matters across lines");
	qmap_drop(out);

	stoma_index(db, "title", "r28", "Atenção Coração");
	n = stoma_query_phrase(db, "title", "atenção coração", out, &handled);
	CHECK(n == 1 && hd_has(out, "r28"), "phrase with accents matches");
	qmap_drop(out);
	n = stoma_query_phrase(db, "title", "coracao atencao", out, &handled);
	CHECK(n == 0, "phrase accent-sensitive");
	qmap_drop(out);

	stoma_index(db, "title", "r29", "Morning Dawn");
	n = stoma_query_phrase(db, "title", "morning dawn", out, &handled);
	CHECK(n == 1 && hd_has(out, "r29"), "phrase case-insensitive");
	qmap_drop(out);

	n = stoma_query_phrase(db, "title", "dawn", out, &handled);
	CHECK(handled == 1 && n == 3 && hd_has(out, "r24") &&
	              hd_has(out, "r25") && hd_has(out, "r29"),
	      "single-token phrase equals AND");
	qmap_drop(out);

	n = stoma_query_phrase(db, "title", "blue,  dawn", out, &handled);
	CHECK(n == 1 && hd_has(out, "r24"), "phrase normalizes punct+spaces");
	qmap_drop(out);

	n = stoma_query_phrase(db, "title", "---", out, &handled);
	CHECK(handled == 0 && n == 0, "zero-token phrase no-op");
	qmap_drop(out);

	n = stoma_query_phrase(db, "title", "nowhere at all", out, &handled);
	CHECK(handled == 1 && n == 0, "phrase no match -> 0");
	qmap_drop(out);

	/* 30-35. recall-kernel adapter (rec_axis_fill_tokens / stoma_rank) on a
	 * dedicated decimal-id db so the raw r1-r29 universe is untouched. */
	rdb = stoma_open(0);
	if (rdb) {
		/* 30. fill-phrase=0 == stoma_query on decimal ids 10-17 */
		stoma_index(rdb, "title", "10", "Starlight of the night");
		stoma_index(rdb, "title", "11", "Station one");
		stoma_index(rdb, "title", "12", "st");
		stoma_index(rdb, "title", "13", "A Dark Night");
		stoma_index(rdb, "title", "14", "Black Star");
		stoma_index(rdb, "title", "15", "1984");
		stoma_index(rdb, "title", "16", "Pão de Açúcar");
		stoma_index(rdb, "title", "17", "Estrela negra");
		{
			static const rec_ref_t e10[] = { 10, 13 };
			static const rec_ref_t e11[] = { 10, 11, 12, 14 };
			static const rec_ref_t e14[] = { 14 };
			static const rec_ref_t e15[] = { 15 };
			static const rec_ref_t e16[] = { 16 };
			static const rec_ref_t e17[] = { 17 };
			/* zero-token/empty queries: handled==0 */
			check_row_fill(out, "night", 1, e10, 2);
			check_row_fill(out, "st", 1, e11, 4);
			check_row_fill(out, "black star", 1, e14, 1);
			check_row_fill(out, "star black", 1, e14, 1);
			check_row_fill(out, "1984", 1, e15, 1);
			check_row_fill(out, "19", 1, e15, 1);
			check_row_fill(out, "pão", 1, e16, 1);
			check_row_fill(out, "pao", 1, NULL, 0);
			check_row_fill(out, "estrela", 1, e17, 1);
			check_row_fill(out, "nowhere", 1, NULL, 0);
			check_row_fill(out, "---", 0, NULL, 0);
			check_row_fill(out, "", 0, NULL, 0);
		}

		/* 31. add ids 18-23; fill-phrase=1 == stoma_query_phrase, and
		 * fill-phrase=0 == stoma_query */
		stoma_index(rdb, "title", "18", "Blue Dawn");
		stoma_index(rdb, "title", "19", "Blue summer of the dawn");
		stoma_index(rdb, "title", "20", "Atenção Coração");
		stoma_index(rdb, "title", "21", "blackstar manor");
		stoma_index(rdb, "title", "22", "la la la");
		stoma_index(rdb, "title", "23", "");
		{
			static const rec_ref_t e18[] = { 18 };
			static const rec_ref_t e1819[] = { 18, 19 };
			static const rec_ref_t e19[] = { 19 };
			static const rec_ref_t e20[] = { 20 };
			static const rec_ref_t e21[] = { 21 };
			static const rec_ref_t e22[] = { 22 };
			static const struct {
				const char *q;
				const rec_ref_t *e0;
				size_t n0;
				const rec_ref_t *e1;
				size_t n1;
			} rows[] = {
				{ "blue dawn", e1819, 2, e18, 1 },
				{ "blue summer", e19, 1, e19, 1 },
				{ "dawn blue", e1819, 2, NULL, 0 },
				{ "black manor", e21, 1, e21, 1 },
				{ "star manor", NULL, 0, NULL, 0 },
				{ "atenção coração", e20, 1, e20, 1 },
				{ "coracao atencao", NULL, 0, NULL, 0 },
				{ "blue", e1819, 2, e1819, 2 },
				{ "la", e22, 1, e22, 1 },
				{ "la la", e22, 1, e22, 1 },
				{ "dawn", e1819, 2, e1819, 2 },
			};
			size_t i;
			int p;

			for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
				for (p = 0; p < 2; p++) {
					int handled = 0;
					uint32_t nn;
					rec_set_t *fs;
					char name[128];
					const rec_ref_t *e =
					        p ? rows[i].e1 : rows[i].e0;
					size_t ne = p ? rows[i].n1
					              : rows[i].n0;

					qmap_drop(out);
					if (p)
						nn = stoma_query_phrase(
						        rdb, "title", rows[i].q,
						        out, &handled);
					else
						nn = stoma_query(
						        rdb, "title", rows[i].q,
						        out, &handled);
					fs = fill_once(rdb, "title", rows[i].q,
					               p);
					snprintf(name, sizeof(name),
					         "fill==query_phrase=%d '%s'", p,
					         rows[i].q);
					CHECK(fs && handled == 1 && nn == ne &&
					              rec_set_count(fs) == ne &&
					              qmap_equals_set(out, fs) &&
					              rec_sorted_unique(fs),
					      name);
					if (fs) {
						const rec_ref_t *sb =
						        rec_set_at(fs);
						size_t j;

						for (j = 0; j < ne; j++)
							CHECK(sb[j] == e[j],
							      "fill member order");
						rec_set_free(fs);
					}
				}
		}

		/* 32. seal + additivity */
		{
			rec_set_t *fs = fill_once(rdb, "title", "night", 0);

			CHECK(fs && rec_set_count(fs) == 2 && rec_has(fs, 10) &&
			              rec_has(fs, 13) && rec_sorted_unique(fs),
			      "fill night count+seal");
			CHECK(fs && rec_axis_fill_tokens(rdb, "title", "estrela", 0,
			                                 fs) == 0 &&
			              rec_set_count(fs) == 3 && rec_has(fs, 17) &&
			              rec_sorted_unique(fs),
			      "fill additive union");
			CHECK(fs && rec_axis_fill_tokens(rdb, "title", "night", 0,
			                                 fs) == 0 &&
			              rec_set_count(fs) == 3,
			      "fill reseal dedup stable");
			rec_set_push(fs, 999);
			CHECK(fs && rec_axis_fill_tokens(rdb, "title", "night", 0,
			                                 fs) == 0 &&
			              rec_set_count(fs) == 4 && rec_has(fs, 999) &&
			              rec_sorted_unique(fs),
			      "fill keeps pre-existing refs");
			rec_set_free(fs);
			fs = fill_once(rdb, "title", "black star", 0);
			CHECK(fs && rec_set_count(fs) == 1 && rec_has(fs, 14),
			      "fill multi-token dedup");
			rec_set_free(fs);
		}

		/* 33. arg validation + non-decimal store */
		{
			rec_set_t *fs = rec_set_new();
			float sc;
			struct stoma_rank_ctx c0;

			CHECK(rec_axis_fill_tokens(NULL, "title", "x", 0, fs) ==
			              -1,
			      "fill null db");
			CHECK(rec_axis_fill_tokens(rdb, NULL, "x", 0, fs) == -1,
			      "fill null field");
			CHECK(rec_axis_fill_tokens(rdb, "title", NULL, 0, fs) ==
			              -1,
			      "fill null query");
			CHECK(rec_axis_fill_tokens(rdb, "title", "x", 0, NULL) ==
			              -1,
			      "fill null out");
			rec_set_free(fs);
			c0.db = NULL;
			c0.field = NULL;
			c0.matched = 0;
			CHECK(stoma_rank(NULL, 1, &sc) == -1, "rank null ctx");
			CHECK(stoma_rank(&c0, 1, &sc) == -1, "rank null db");
			c0.db = rdb;
			CHECK(stoma_rank(&c0, 1, &sc) == -1, "rank null field");
			c0.field = "title";
			CHECK(stoma_rank(&c0, 1, NULL) == -1, "rank null score");
		}
		{
			stoma_db_t *xdb = stoma_open(0);
			rec_set_t *fs;

			stoma_index(xdb, "title", "r7", "alpha beta");
			stoma_index(xdb, "title", "12x", "alpha beta");
			fs = fill_once(xdb, "title", "alpha", 0);
			CHECK(fs == NULL, "fill rejects non-decimal row id");
			if (fs)
				rec_set_free(fs);
			stoma_close(xdb);
		}

		/* 34. stoma_rank numbers */
		{
			struct stoma_rank_ctx ctx = { rdb, "title", 1 };
			float sc;

			CHECK(stoma_rank(&ctx, 10, &sc) == 0 &&
			              float_close(sc, 0.25f),
			      "rank 10 1/4");
			CHECK(stoma_rank(&ctx, 13, &sc) == 0 &&
			              float_close(sc, 1.0f / 3.0f),
			      "rank 13 1/3");
			CHECK(stoma_rank(&ctx, 15, &sc) == 0 &&
			              float_close(sc, 1.0f),
			      "rank 15 1/1");
			CHECK(stoma_rank(&ctx, 17, &sc) == 0 &&
			              float_close(sc, 0.5f),
			      "rank 17 1/2");
			{
				struct stoma_rank_ctx ctx2 = { rdb, "title", 2 };

				CHECK(stoma_rank(&ctx2, 15, &sc) == 0 &&
				              float_close(sc, 2.0f),
				      "rank matched override >1");
			}
			CHECK(stoma_rank(&ctx, 999, &sc) == -1,
			      "rank missing doc");
			{
				struct stoma_rank_ctx ctxb = { rdb, "bogus", 1 };

				CHECK(stoma_rank(&ctxb, 10, &sc) == -1,
				      "rank missing field");
			}
			CHECK(stoma_rank(&ctx, 23, &sc) == -1,
			      "rank zero-token doc");
			CHECK(stoma_rank(&ctx, 10, &sc) == 0 &&
			              float_close(sc, 0.25f),
			      "rank round-trip stable");
		}

		/* 35. kernel integration (rec_set join + rec_rank loop) */
		{
			rec_set_t *A = fill_once(rdb, "title", "night", 0);
			rec_set_t *B = rec_set_new();
			rec_set_t *I = rec_set_new();
			rec_set_t *D = rec_set_new();
			rec_set_t *U = rec_set_new();
			rec_ref_t refs[2];
			float scores[2];
			size_t nr;
			size_t i;

			rec_set_push(B, 9);
			rec_set_push(B, 10);
			rec_set_push(B, 13);
			rec_set_push(B, 14);
			rec_set_seal(B);
			CHECK(rec_set_intersect(I, A, B) == 0 &&
			              rec_set_count(I) == 2 && rec_has(I, 10) &&
			              rec_has(I, 13),
			      "kernel intersect");
			CHECK(rec_set_subtract(D, B, A) == 0 &&
			              rec_set_count(D) == 2 && rec_has(D, 9) &&
			              rec_has(D, 14),
			      "kernel subtract");
			CHECK(rec_set_union(U, B, A) == 0 &&
			              rec_set_count(U) == 4 && rec_has(U, 9) &&
			              rec_has(U, 10) && rec_has(U, 13) &&
			              rec_has(U, 14),
			      "kernel union");
			{
				rec_rank_t *rk = rec_rank_new(2, 0.0f);
				struct stoma_rank_ctx ctx = { rdb, "title", 1 };
				float sc;

				for (i = 0; i < rec_set_count(A); i++)
					if (stoma_rank(&ctx, rec_set_at(A)[i],
					               &sc) == 0)
						rec_rank_push(rk, rec_set_at(A)[i],
						              sc);
				nr = rec_rank_sorted(rk, refs, scores);
				CHECK(nr == 2 && refs[0] == 13 && refs[1] == 10 &&
				              float_close(scores[0],
				                          1.0f / 3.0f) &&
				              float_close(scores[1], 0.25f),
				      "kernel rank loop best-first");
				rec_rank_free(rk);
			}
			{
				rec_rank_t *rk = rec_rank_new(1, 0.31f);
				struct stoma_rank_ctx ctx = { rdb, "title", 1 };
				float sc;

				for (i = 0; i < rec_set_count(A); i++)
					if (stoma_rank(&ctx, rec_set_at(A)[i],
					               &sc) == 0)
						rec_rank_push(rk, rec_set_at(A)[i],
						              sc);
				nr = rec_rank_sorted(rk, refs, scores);
				CHECK(nr == 1 && refs[0] == 13,
				      "kernel min_score filters");
				rec_rank_free(rk);
			}
			rec_set_free(A);
			rec_set_free(B);
			rec_set_free(I);
			rec_set_free(D);
			rec_set_free(U);
		}

		/* 36. rec_query "stoma" axis registration: registered fill/rank
		 * dispatch through the axis table must equal the direct
		 * rec_axis_fill_tokens / stoma_rank calls. */
		{
			int slot = -1;
			int i;
			const rec_axis_t *axis;

			for (i = 0; i < rec_axis_count(); i++) {
				axis = rec_axis_get(i);
				if (axis && !strcmp(axis->name, "stoma")) {
					slot = i;
					break;
				}
			}
			CHECK(slot >= 0, "stoma axis registered");
			axis = slot >= 0 ? rec_axis_get(slot) : NULL;
			CHECK(axis && axis->fill && axis->rank && axis->decode,
			      "stoma axis has fill+rank+decode");
			CHECK(axis && axis->ctx == NULL,
			      "stoma axis ctx NULL before set_ctx");
			CHECK(slot < 0 || rec_axis_set_ctx(slot, rdb) == 0,
			      "stoma axis set_ctx ok");
			axis = slot >= 0 ? rec_axis_get(slot) : NULL;
			CHECK(axis && axis->ctx == rdb,
			      "stoma axis ctx == rdb after set_ctx");

			if (axis) {
				void *p = rec_axis_decode(
				        slot, "field=title query=night phrase=0");
				rec_set_t *direct = fill_once(rdb, "title",
				                              "night", 0);
				rec_set_t *via = rec_set_new();

				CHECK(p != NULL, "decode basic params");
				CHECK(via && p &&
				              axis->fill(axis->ctx, p, via) ==
				                      0,
				      "registered fill via axis table ok");
				CHECK(direct && via &&
				              rec_set_count(direct) ==
				                      rec_set_count(via) &&
				              (rec_set_count(direct) == 0 ||
				               !memcmp(rec_set_at(direct),
				                       rec_set_at(via),
				                       rec_set_count(direct) *
				                               sizeof(rec_ref_t))),
				      "registered fill == direct rec_axis_fill_tokens");
				if (direct)
					rec_set_free(direct);
				if (via)
					rec_set_free(via);
			}

			if (axis) {
				/* phrase + quoted multi-word query */
				void *pp = rec_axis_decode(
				        slot,
				        "field=title query='black star' phrase=1");
				rec_set_t *direct = fill_once(rdb, "title",
				                              "black star", 1);
				rec_set_t *via = rec_set_new();

				CHECK(pp != NULL, "decode quoted phrase params");
				CHECK(via && pp &&
				              axis->fill(axis->ctx, pp, via) ==
				                      0,
				      "registered fill (phrase) ok");
				CHECK(direct && via &&
				              rec_set_count(direct) ==
				                      rec_set_count(via) &&
				              (rec_set_count(direct) == 0 ||
				               !memcmp(rec_set_at(direct),
				                       rec_set_at(via),
				                       rec_set_count(direct) *
				                               sizeof(rec_ref_t))),
				      "registered fill == direct (phrase, quoted query)");
				if (direct)
					rec_set_free(direct);
				if (via)
					rec_set_free(via);
			}

			if (axis) {
				/* rank: matched=1, ref 13 ("A Dark Night") */
				void *rp = rec_axis_decode(
				        slot, "field=title matched=1");
				struct stoma_rank_ctx dctx = { rdb, "title", 1 };
				float direct_sc = 0.0f, via_sc = 0.0f;
				int drc, vrc;

				CHECK(rp != NULL, "decode rank params");
				drc = stoma_rank(&dctx, 13, &direct_sc);
				vrc = rp ? axis->rank(axis->ctx, rp, 13,
				                      &via_sc)
				         : -1;
				CHECK(drc == 0 && vrc == 0 &&
				              float_close(direct_sc, via_sc),
				      "registered rank == direct stoma_rank");
			}

			CHECK(rec_axis_decode(slot, "") != NULL,
			      "decode empty string still allocates defaults");
			CHECK(rec_axis_decode(-1, "field=x") == NULL,
			      "decode invalid slot -> NULL");
		}

		/* 36b. D14 rec_axis_cli_options / rec_axis_config_arg
		 * convention: stoma declares the generic `query` flag; decode
		 * merges it ONLY in the non-NULL, non-empty leaf path when the
		 * leaf omits query= (bare stoma stays not-searchable, and an
		 * empty spec keeps query=""). */
		{
			int slot = -1;
			int i;
			const rec_axis_t *axis;
			extern const struct stoma_cli_read {
				const char *name;
				int has_arg;
				const char *help;
			} *rec_axis_cli_options(void);
			extern int rec_axis_config_arg(const char *, const char *);
			struct stoma_params_read {
				const char *field;
				const char *query;
				int phrase;
				size_t matched;
			};
			const struct stoma_cli_read *o;
			int n = 0;

			for (i = 0; i < rec_axis_count(); i++) {
				axis = rec_axis_get(i);
				if (axis && !strcmp(axis->name, "stoma")) {
					slot = i;
					break;
				}
			}
			CHECK(slot >= 0, "stoma axis registered (cli test)");
			axis = slot >= 0 ? rec_axis_get(slot) : NULL;

			o = rec_axis_cli_options();
			while (o && o[n].name)
				n++;
			CHECK(n == 1, "cli table: query only");
			CHECK(o && !strcmp(o[0].name, "query"),
			      "option is query");
			CHECK(o && o[0].has_arg == 1, "query takes a value");
			CHECK(rec_axis_config_arg("query", NULL) != 0,
			      "query NULL rejected");
			CHECK(rec_axis_config_arg("bogus", "x") != 0,
			      "unknown option rejected");

			/* bare NULL spec stays NULL even with --query */
			CHECK(rec_axis_config_arg("query", "harbor") == 0,
			      "config_arg query ok");
			CHECK(!axis || axis->decode(NULL) == NULL,
			      "bare stoma with --query stays NULL");

			/* leaf omitting query= merges the CLI query */
			{
				void *p = slot < 0 ? NULL
				        : rec_axis_decode(
					          slot, "field=title matched=1");
				CHECK(p != NULL, "decode merges CLI query");
				if (p) {
					struct stoma_params_read *sp = p;
					CHECK(sp->field != NULL &&
					              !strcmp(sp->field, "title"),
					      "field from leaf");
					CHECK(sp->query != NULL &&
					              !strcmp(sp->query, "harbor"),
					      "query from CLI");
				}
			}

			/* a leaf query= wins over --query */
			{
				void *p = slot < 0 ? NULL
				        : rec_axis_decode(
					          slot, "field=title query=leaftext");
				CHECK(p != NULL, "decode leaf query present");
				if (p) {
					struct stoma_params_read *sp = p;
					CHECK(sp->query != NULL &&
					              !strcmp(sp->query, "leaftext"),
					      "leaf query beats --query");
				}
			}

			/* empty spec: allocates, keeps query="" (no merge) */
			{
				void *p = slot < 0 ? NULL
				        : rec_axis_decode(slot, "");
				CHECK(p != NULL,
				      "decode empty string allocates (cli)");
				if (p) {
					struct stoma_params_read *sp = p;
					CHECK(sp->query != NULL &&
					              sp->query[0] == '\0',
					      "empty spec keeps query empty");
				}
			}
		}

		/* 37. rec_axis_open (RECALL-KERNEL.md): opens a fresh
		 * stoma index from a decimal mask spec and returns a ctx
		 * usable directly through the registered axis. */
		{
			int slot = -1;
			int i;
			const rec_axis_t *axis;
			void *ctx;
			stoma_db_t *odb;

			for (i = 0; i < rec_axis_count(); i++) {
				axis = rec_axis_get(i);
				if (axis && !strcmp(axis->name, "stoma")) {
					slot = i;
					break;
				}
			}
			CHECK(slot >= 0, "stoma axis registered (open test)");

			ctx = rec_axis_open("");
			CHECK(ctx != NULL, "rec_axis_open(\"\") returns a ctx");
			odb = ctx;
			stoma_index(odb, "title", "100", "hello world");
			CHECK(slot < 0 || rec_axis_set_ctx(slot, ctx) == 0,
			      "rec_axis_open ctx bindable via set_ctx");
			axis = slot >= 0 ? rec_axis_get(slot) : NULL;
			CHECK(axis && axis->ctx == ctx,
			      "axis ctx == rec_axis_open() result");

			if (axis) {
				void *p = rec_axis_decode(
				        slot, "field=title query=hello phrase=0");
				rec_set_t *direct = fill_once(odb, "title",
				                              "hello", 0);
				rec_set_t *via = rec_set_new();

				CHECK(p != NULL, "decode params (open-backed db)");
				CHECK(via && p &&
				              axis->fill(axis->ctx, p, via) ==
				                      0,
				      "registered fill via rec_axis_open ctx ok");
				CHECK(direct && via &&
				              rec_set_count(direct) ==
				                      rec_set_count(via) &&
				              rec_set_count(direct) == 1,
				      "registered fill == direct fill (open-backed db)");
				if (direct)
					rec_set_free(direct);
				if (via)
					rec_set_free(via);
			}

			stoma_close(odb);
		}

		/* 38. stoma_index_ref: rec_ref_t-native write path, id-
		 * uniformity boundary shared with libjoint/libsepal/libislet
		 * rec_axis_fill_* functions. rec_ref_t is u32; UINT32_MAX
		 * round-trips index -> fill -> rank exactly. The string path
		 * still accepts any decimal row_id at INDEX time, but the fill
		 * aborts (> UINT32_MAX) rather than truncate to a wrong ref --
		 * the strict-decimal contract extended to the u32 ceiling. */
		{
			rec_ref_t big = UINT32_MAX; /* u32 ceiling */

			CHECK(stoma_index_ref(rdb, "title", big,
			                       "Refwise Uniform") == 0,
			      "stoma_index_ref max-u32 ref indexes ok");
			{
				rec_set_t *fs = fill_once(rdb, "title",
				                          "refwise", 0);

				CHECK(fs && rec_set_count(fs) == 1 &&
				              rec_has(fs, big),
				      "fill_tokens sees stoma_index_ref'd max-u32 ref");
				if (fs)
					rec_set_free(fs);
			}
			{
				struct stoma_rank_ctx ctx = { rdb, "title", 1 };
				float sc;

				CHECK(stoma_rank(&ctx, big, &sc) == 0 &&
				              float_close(sc, 0.5f),
				      "stoma_rank on stoma_index_ref'd max-u32 ref");
			}
			CHECK(stoma_index_ref(NULL, "title", big, "x") == -1,
			      "stoma_index_ref null db");
			CHECK(stoma_index_ref(rdb, NULL, big, "x") == -1,
			      "stoma_index_ref null field");
			CHECK(stoma_index_ref(rdb, "title", big, NULL) == -1,
			      "stoma_index_ref null value");

			/* guard: a >UINT32_MAX decimal row_id is storable via the
			 * string path (documented), but the fill must abort instead
			 * of truncating. */
			CHECK(stoma_index(rdb, "title", "5000000000",
			                   "Outer Reach") == 0,
			      "stoma_index stores >UINT32_MAX decimal row_id (string path)");
			{
				rec_set_t *fs = rec_set_new();
				int rc = rec_axis_fill_tokens(rdb, "title",
				                              "outer", 0, fs);

				CHECK(rc == -1,
				      "fill aborts on >UINT32_MAX row_id (no truncation)");
				if (fs)
					rec_set_free(fs);
			}
			{
				unsigned hd = qmap_open(NULL, NULL, QM_STR,
				                        QM_STR, 0xFF, 0);
				int qhandled = 0;
				uint32_t n = stoma_query(rdb, "title", "outer",
				                         hd, &qhandled);

				CHECK(n == 1 && qhandled == 1 &&
				              hd_has(hd, "5000000000"),
				      "raw stoma_query still serves >UINT32_MAX row_id (string path)");
				qmap_close(hd);
			}
		}
		stoma_close(rdb);
	}

	/* 39. stoma_unindex / stoma_unindex_ref: the differential inverse
	 * (Phase 2A 2A-2). index -> unindex must leave zero residual
	 * postings for the row — every query token of the original doc must
	 * no longer reach the row, while shared tokens still reach the
	 * other rows — plus no doc side-table entry, per-field isolation,
	 * and idempotent 0 on absent rows. Runs on a fresh db (the rdb
	 * universe above is closed at this point; rdb is reused as the
	 * fixture handle and cleared at the end). */
	{
		unsigned uhd = qmap_open(NULL, NULL, QM_STR, QM_STR, 0xFF,
		                         0);
		int qh = 0;
		uint32_t n;
		struct stoma_rank_ctx rctx;
		float sc;

		/* fresh db: the rdb universe above is closed here */
		rdb = stoma_open(0);
		CHECK(rdb != NULL, "unindex fixture db opens");

		/* idempotent no-op before anything is stored */
		CHECK(stoma_unindex(rdb, "u", "ghost") == 0,
		      "unindex absent row -> 0");
		CHECK(stoma_unindex_ref(rdb, "u", 777) == 0,
		      "unindex_ref absent row -> 0");

		CHECK(stoma_index(rdb, "u", "u1",
		                   "Harbor Light Beacons") == 0,
		      "unindex fixture u1");
		CHECK(stoma_index(rdb, "u", "u2",
		                   "Harbor Charts Beacons") == 0,
		      "unindex fixture u2");
		CHECK(stoma_index(rdb, "u", "u3",
		                   "Distant Star") == 0,
		      "unindex fixture u3");
		/* same row in another field must survive the u-unindex */
		CHECK(stoma_index(rdb, "v", "u2",
		                   "Harbor Charts Beacons") == 0,
		      "unindex fixture other field");

		CHECK(stoma_unindex(rdb, "u", "u2") == 0,
		      "stoma_unindex u2 ok");

		/* exact residual sweep: posting keys only ever encode the
		 * doc's own tokens, so querying every original token and
		 * finding no trace of the row proves zero residual
		 * postings. Shared tokens must still reach the other row. */
		qmap_drop(uhd);
		n = stoma_query(rdb, "u", "harbor", uhd, &qh);
		CHECK(qh == 1 && n == 1 && hd_has(uhd, "u1") &&
		              !hd_has(uhd, "u2"),
		      "unindex: shared token keeps u1, drops u2");
		qmap_drop(uhd);
		n = stoma_query(rdb, "u", "beacons", uhd, &qh);
		CHECK(qh == 1 && n == 1 && hd_has(uhd, "u1") &&
		              !hd_has(uhd, "u2"),
		      "unindex: shared token beacons keeps u1, drops u2");
		qmap_drop(uhd);
		n = stoma_query(rdb, "u", "charts", uhd, &qh);
		CHECK(qh == 1 && n == 0, "unindex: u2-only token gone");
		qmap_drop(uhd);
		n = stoma_query(rdb, "u", "harbor beacons", uhd, &qh);
		CHECK(qh == 1 && n == 1 && hd_has(uhd, "u1") &&
		              !hd_has(uhd, "u2"),
		      "unindex: AND query keeps u1, drops u2");
		qmap_drop(uhd);

		/* untouched row and other field survive */
		n = stoma_query(rdb, "u", "distant star", uhd, &qh);
		CHECK(qh == 1 && n == 1 && hd_has(uhd, "u3"),
		      "unindex leaves u3 alone");
		qmap_drop(uhd);
		n = stoma_query(rdb, "v", "charts", uhd, &qh);
		CHECK(qh == 1 && n == 1 && hd_has(uhd, "u2"),
		      "unindex is per-field (v/u2 survives)");

		/* second unindex is the idempotent 0 */
		CHECK(stoma_unindex(rdb, "u", "u2") == 0,
		      "unindex twice -> 0");

		/* duplicate-token docs collapse to one posting each and
		 * unindex leaves zero residual for them too */
		CHECK(stoma_index(rdb, "u", "u4", "la la la") == 0,
		      "unindex fixture dup tokens");
		CHECK(stoma_unindex(rdb, "u", "u4") == 0,
		      "unindex dup-token doc ok");
		n = stoma_query(rdb, "u", "la", uhd, &qh);
		CHECK(qh == 1 && n == 0, "unindex: dup-token posting gone");

		/* the decimal path: rank reads the doc side-table directly,
		 * so rank --1 after unindex_ref proves the doc entry is
		 * gone (and reindexing restores exactly one entry). */
		CHECK(stoma_index_ref(rdb, "u", 778,
		                       "Beacon Harbor Words") == 0,
		      "unindex fixture decimal ref");
		rctx.db = rdb;
		rctx.field = "u";
		rctx.matched = 1;
		CHECK(stoma_rank(&rctx, 778, &sc) == 0,
		      "rank sees indexed decimal ref");
		CHECK(stoma_unindex_ref(rdb, "u", 778) == 0,
		      "stoma_unindex_ref ok");
		CHECK(stoma_rank(&rctx, 778, &sc) == -1,
		      "rank misses after unindex_ref (doc entry gone)");
		n = stoma_query(rdb, "u", "beacon", uhd, &qh);
		CHECK(n == 0 || !hd_has(uhd, "778"),
		      "unindex_ref: postings gone");
		CHECK(stoma_unindex_ref(rdb, "u", 778) == 0,
		      "unindex_ref twice -> 0");

		/* NULL contracts mirror stoma_index */
		CHECK(stoma_unindex(NULL, "u", "u1") == -1,
		      "unindex null db");
		CHECK(stoma_unindex(rdb, NULL, "u1") == -1,
		      "unindex null field");
		CHECK(stoma_unindex(rdb, "u", NULL) == -1,
		      "unindex null row");
		CHECK(stoma_unindex_ref(NULL, "u", 1) == -1,
		      "unindex_ref null db");
		CHECK(stoma_unindex_ref(rdb, NULL, 1) == -1,
		      "unindex_ref null field");

		qmap_close(uhd);
		stoma_close(rdb);
		rdb = NULL;
	}
	stoma_close(db);
	qmap_close(out);

	printf("Results: %d/%d passed", total - failures, total);
	if (failures > 0)
		printf(", %d FAILED", failures);
	printf("\n");
	return failures > 0 ? 1 : 0;
}
