#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ttypt/corm.h>
#include <ttypt/rec.h>
#include "stoma/stoma.h"

/* 2A-2 adapter contract: rec_axis_store / rec_axis_unstore /
 * rec_axis_readback on the canonical "text" field.
 *
 * The consumer passes (ref, value) blindly; stoma stores the WHOLE value
 * string's text under STOMA_AXIS_TEXT_FIELD (replace-in-place: a ref owns
 * exactly one doc). Read-back returns the stored (folded — lowercased,
 * accent-preserving) text as one NUL-joined entry; n_out counts the
 * display chars (libsepal convention). Zero new stored state; memory-only.
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

int main(void)
{
	stoma_db_t *db = stoma_open(0);
	char *blob;
	size_t n;

	if (!db) {
		printf("setup failed\n");
		return 1;
	}

	/* 1. store the whole string; queryable under the canonical field
	 * (exact + prefix), invisible under any other field. */
	CHECK(rec_axis_store(db, NULL, 7, "Beacon Harbor") == 0,
	      "store whole string ok");
	CHECK(text_finds(db, "harbor", 7), "store: exact token finds ref");
	CHECK(text_finds(db, "bea", 7), "store: prefix finds ref");
	{
		unsigned hd = corm_open(NULL, NULL, CM_STR, CM_STR, 0xFF,
		                        0);
		int handled = 0;
		uint32_t m = stoma_query(db, "title", "beacon", hd,
		                         &handled);

		CHECK(handled == 1 && m == 0, "store lands under text only");
		corm_close(hd);
	}

	/* 2. readback is the stored (folded) text; n_out counts chars. */
	CHECK(rec_axis_store(db, NULL, 8, "Beacon HARBOR Pão") == 0,
	      "store mixed-case accented ok");
	blob = NULL;
	n = 0;
	CHECK(rec_axis_readback(db, 8, &blob, &n) == 0,
	      "readback ok");
	CHECK(blob && strcmp(blob, "beacon harbor pão") == 0,
	      "readback is the folded text (lowercased, accent kept)");
	CHECK(blob && n == strlen(blob), "readback n_out counts chars");
	free(blob);

	/* 3. absent ref reads back NULL/0, still returns 0. */
	blob = (char *)0x1;
	n = 9;
	CHECK(rec_axis_readback(db, 31337, &blob, &n) == 0,
	      "readback absent -> 0");
	CHECK(blob == NULL, "readback absent -> NULL blob");
	CHECK(n == 0, "readback absent -> 0 chars");

	/* 4. replace-in-place: the second store erases the first, so a
	 * ref owns exactly one doc. */
	CHECK(rec_axis_store(db, NULL, 9, "foo bar") == 0,
	      "store first value");
	CHECK(text_finds(db, "foo", 9), "first value findable");
	CHECK(rec_axis_store(db, NULL, 9, "baz quux") == 0,
	      "store replaces in place");
	CHECK(text_misses(db, "foo", 9), "old tokens gone after replace");
	CHECK(text_finds(db, "baz", 9), "new tokens findable");
	blob = NULL;
	n = 0;
	CHECK(rec_axis_readback(db, 9, &blob, &n) == 0 &&
	              blob && strcmp(blob, "baz quux") == 0,
	      "readback shows the replacement");
	free(blob);

	/* 5. unstore removes the entry; absent is the idempotent 0. */
	CHECK(rec_axis_unstore(db, 9) == 0, "unstore ok");
	CHECK(text_misses(db, "baz", 9), "unstore: tokens gone");
	blob = (char *)0x1;
	n = 1;
	CHECK(rec_axis_readback(db, 9, &blob, &n) == 0 && blob == NULL &&
	              n == 0,
	      "unstore: readback absent");
	CHECK(rec_axis_unstore(db, 9) == 0, "unstore twice -> 0");
	CHECK(rec_axis_unstore(db, 424242) == 0,
	      "unstore never-stored -> 0");

	/* 6. per-ref isolation: same text on two refs, forget one. */
	CHECK(rec_axis_store(db, NULL, 11, "alpha one") == 0,
	      "store ref 11");
	CHECK(rec_axis_store(db, NULL, 12, "alpha one") == 0,
	      "store ref 12 same text");
	CHECK(rec_axis_unstore(db, 11) == 0, "unstore ref 11");
	CHECK(text_misses(db, "alpha", 11), "ref 11 gone");
	CHECK(text_finds(db, "alpha", 12), "ref 12 untouched");

	/* 7. NULL / empty value contracts (errno EINVAL). */
	errno = 0;
	CHECK(rec_axis_store(db, NULL, 1, NULL) == -1 &&
	              errno == EINVAL,
	      "store null value -> EINVAL");
	errno = 0;
	CHECK(rec_axis_store(db, NULL, 1, "") == -1 && errno == EINVAL,
	      "store empty value -> EINVAL");

	/* 8. NULL ctx / NULL out-arg contracts. */
	CHECK(rec_axis_store(NULL, NULL, 1, "x") == -1,
	      "store null ctx -> -1");
	CHECK(rec_axis_unstore(NULL, 1) == -1, "unstore null ctx -> -1");
	blob = (char *)0x1;
	n = 1;
	CHECK(rec_axis_readback(NULL, 1, &blob, &n) == -1 &&
	              blob == NULL && n == 0,
	      "readback null ctx clears outs, -> -1");
	blob = (char *)0x1;
	n = 1;
	errno = 0;
	CHECK(rec_axis_readback(db, 12, NULL, &n) == -1 &&
	              errno == EINVAL,
	      "readback null blob_out -> EINVAL");
	errno = 0;
	CHECK(rec_axis_readback(db, 12, &blob, NULL) == -1 &&
	              errno == EINVAL,
	      "readback null n_out -> EINVAL");
	free(blob);

	stoma_close(db);

	printf("Results: %d/%d passed", total - failures, total);
	if (failures > 0)
		printf(", %d FAILED", failures);
	printf("\n");
	return failures > 0 ? 1 : 0;
}
