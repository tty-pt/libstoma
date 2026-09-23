#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ttypt/corm.h>
#include <ttypt/rec.h>
#include "stoma/stoma.h"

/* 2A-5 stoma — memory+rebuild round-trip.
 *
 * Memory-only, never stale: rebuilt from primary strings. Proves the
 * documented lifecycle plus its complements:
 *
 *   store (corpus {(1,"Beacon Harbor lights"),(2,"Beacon AND Pão"),
 *          (3,"alpha omega")}) → query "beacon" finds {1,2} ("harbor" finds 1)
 *   unstore(1) → "harbor" misses, "beacon" = {2} only (misses 1)
 *   rebuild (fresh open, re-index corpus minus primary-dropped 3
 *          → {(1,"Beacon Harbor lights"),(2,"Beacon AND Pão")})
 *        → "beacon" = {1,2} again (unstored-but-still-in-primary returns —
 *          never-stale), "harbor" = {1} again, "omega"/"alpha"+"omega"
 *          stays gone (primary-dropped stays gone), plus rebuild-vs-
 *          incremental parity and readback checks.
 */

static int failures = 0;
static int total = 0;

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
	return corm_get(hd, row) != NULL;
}

static int text_finds(stoma_db_t *db, const char *tok, rec_ref_t ref)
{
	unsigned hd = corm_open(NULL, NULL, CM_STR, CM_STR, 0xFF, 0);
	int handled = 0;
	char rid[24];
	uint32_t n;
	int found = 0;
	if (!hd)
		return 0;
	snprintf(rid, sizeof(rid), "%llu", (unsigned long long)ref);
	n = stoma_query(db, STOMA_AXIS_TEXT_FIELD, tok, hd, &handled);
	found = handled == 1 && n >= 1 && hd_has(hd, rid);
	corm_close(hd);
	return found;
}

static int text_misses(stoma_db_t *db, const char *tok, rec_ref_t ref)
{
	unsigned hd = corm_open(NULL, NULL, CM_STR, CM_STR, 0xFF, 0);
	int handled = 0;
	char rid[24];
	int miss = 0;
	if (!hd)
		return 0;
	snprintf(rid, sizeof(rid), "%llu", (unsigned long long)ref);
	(void)stoma_query(db, STOMA_AXIS_TEXT_FIELD, tok, hd, &handled);
	miss = handled == 1 && !hd_has(hd, rid);
	corm_close(hd);
	return miss;
}

static int count_hits(stoma_db_t *db, const char *tok)
{
	unsigned hd = corm_open(NULL, NULL, CM_STR, CM_STR, 0xFF, 0);
	int handled = 0;
	uint32_t n = 0;
	if (!hd)
		return -1;
	n = stoma_query(db, STOMA_AXIS_TEXT_FIELD, tok, hd, &handled);
	corm_close(hd);
	if (!handled)
		return 0;
	return (int)n;
}

int main(void)
{
	stoma_db_t *db = NULL;
	stoma_db_t *db2 = NULL;
	char *blob = NULL;
	size_t n = 0;

	/* corpus is the "primary authority" — what rebuild re-derives from */
	struct { rec_ref_t ref; const char *text; } corpus[] = {
		{ 1, "Beacon Harbor lights" },
		{ 2, "Beacon AND Pão" },
		{ 3, "alpha omega" },
	};
	size_t corpus_n = sizeof(corpus) / sizeof(corpus[0]);

	db = stoma_open(0);
	CHECK(db != NULL, "open db");

	/* store all corpus entries */
	for (size_t i = 0; i < corpus_n; i++)
		CHECK(rec_axis_store(db, NULL, corpus[i].ref, corpus[i].text) == 0,
		      "store corpus entry");

	/* query finds: "beacon" is in 1 and 2 (AND, prefix) */
	CHECK(text_finds(db, "beacon", 1), "store: beacon finds 1");
	CHECK(text_finds(db, "beacon", 2), "store: beacon finds 2");
	CHECK(text_misses(db, "beacon", 3), "store: beacon misses 3");
	CHECK(count_hits(db, "beacon") == 2, "store: beacon count 2");
	CHECK(text_finds(db, "harbor", 1), "store: harbor finds 1");
	CHECK(text_misses(db, "harbor", 2), "store: harbor misses 2");
	CHECK(text_finds(db, "alpha", 3), "store: alpha finds 3");
	/* case + prefix: "BEA" should find both 1 and 2 */
	CHECK(text_finds(db, "BEA", 1), "store: prefix case-insensitive 1");
	CHECK(text_finds(db, "BEA", 2), "store: prefix case-insensitive 2");

	/* readback is the folded (lowercased, accent-kept) doc, one entry */
	blob = NULL; n = 0;
	CHECK(rec_axis_readback(db, 1, &blob, &n) == 0 && blob &&
	              strcmp(blob, "beacon harbor lights") == 0,
	      "readback 1 is folded");
	free(blob); blob = NULL;
	CHECK(rec_axis_readback(db, 2, &blob, &n) == 0 && blob &&
	              strcmp(blob, "beacon and pão") == 0,
	      "readback 2 folded, accent kept");
	free(blob); blob = NULL;

	/* unstore 1: only its postings gone, per-ref isolation */
	CHECK(rec_axis_unstore(db, 1) == 0, "unstore 1");
	CHECK(text_misses(db, "harbor", 1), "unstore: harbor misses 1");
	CHECK(text_finds(db, "beacon", 2), "unstore: beacon still finds 2");
	CHECK(text_misses(db, "beacon", 1), "unstore: beacon misses 1");
	CHECK(count_hits(db, "beacon") == 1, "unstore: beacon count 1");
	CHECK(text_finds(db, "alpha", 3), "unstore: 3 untouched");
	blob = (char *)0x1; n = 1;
	CHECK(rec_axis_readback(db, 1, &blob, &n) == 0 && blob == NULL && n == 0,
	      "unstore: readback 1 absent");
	free(blob);

	/* rebuild: fresh open, re-index corpus minus primary-dropped 3 */
	db2 = stoma_open(0);
	CHECK(db2 != NULL, "rebuild: fresh open");
	for (size_t i = 0; i < corpus_n; i++) {
		if (corpus[i].ref == 3)
			continue; /* primary-dropped — not in rebuild corpus */
		CHECK(rec_axis_store(db2, NULL, corpus[i].ref, corpus[i].text) == 0,
		      "rebuild: store");
	}
	/* never-stale: unstored-but-still-in-primary 1 returns */
	CHECK(text_finds(db2, "beacon", 1), "rebuild: beacon finds 1 again");
	CHECK(text_finds(db2, "beacon", 2), "rebuild: beacon finds 2");
	CHECK(count_hits(db2, "beacon") == 2, "rebuild: beacon count 2 again");
	CHECK(text_finds(db2, "harbor", 1), "rebuild: harbor finds 1 again");
	/* primary-dropped stays gone */
	CHECK(text_misses(db2, "alpha", 3), "rebuild: alpha misses 3 (dropped)");
	CHECK(text_misses(db2, "omega", 3), "rebuild: omega misses 3");
	CHECK(text_misses(db2, "alpha", 1), "rebuild: alpha still misses 1");
	CHECK(count_hits(db2, "alpha") == 0, "rebuild: alpha count 0 (3 dropped)");
	/* readback after rebuild matches store */
	blob = NULL; n = 0;
	CHECK(rec_axis_readback(db2, 1, &blob, &n) == 0 && blob &&
	              strcmp(blob, "beacon harbor lights") == 0,
	      "rebuild: readback 1");
	free(blob); blob = NULL;
	CHECK(rec_axis_readback(db2, 2, &blob, &n) == 0 && blob &&
	              strcmp(blob, "beacon and pão") == 0,
	      "rebuild: readback 2");
	free(blob); blob = NULL;
	blob = (char *)0x1; n = 1;
	CHECK(rec_axis_readback(db2, 3, &blob, &n) == 0 && blob == NULL && n == 0,
	      "rebuild: readback 3 absent");

	/* rebuild-vs-incremental parity: fresh incremental of the same
	 * surviving corpus must give the same beacon/harbor sets */
	{
		stoma_db_t *db3 = stoma_open(0);
		CHECK(db3 != NULL, "parity: fresh open");
		CHECK(rec_axis_store(db3, NULL, 1, "Beacon Harbor lights") == 0,
		      "parity: store 1");
		CHECK(rec_axis_store(db3, NULL, 2, "Beacon AND Pão") == 0,
		      "parity: store 2");
		CHECK(count_hits(db2, "beacon") == count_hits(db3, "beacon"),
		      "parity: beacon count matches incremental");
		CHECK(count_hits(db2, "harbor") == count_hits(db3, "harbor"),
		      "parity: harbor count matches incremental");
		CHECK(text_finds(db3, "beacon", 1) == text_finds(db2, "beacon", 1),
		      "parity: beacon finds 1 matches");
		CHECK(text_finds(db3, "beacon", 2) == text_finds(db2, "beacon", 2),
		      "parity: beacon finds 2 matches");
		stoma_close(db3);
	}

	stoma_close(db);
	stoma_close(db2);

	printf("Results: %d/%d passed", total - failures, total);
	if (failures > 0)
		printf(", %d FAILED", failures);
	printf("\n");
	return failures > 0 ? 1 : 0;
}
