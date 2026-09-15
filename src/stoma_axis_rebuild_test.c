/* stoma_axis_rebuild_test.c — 7-AXIS-NAMESPACE-PLAN.md Phase B gate
 * (B-1/B-2): per-primary rebuild binding for rec_axis_open's path branch.
 *
 * Fixture: a temp dir holding two primaries (A.db, B.db) with distinct
 * one-record texts plus both <primary>.roster sidecars; a second temp dir
 * with a single primary (C.db).
 *
 *   B-1: QMAP_AXIS_PRIMARY set → rebuild binds THAT primary only (A's
 *        text found, B's text absent, "rebuilt 1 docs" on stderr).
 *   B-2: env empty + two rosters → valid empty ctx, both texts absent,
 *        loud "N roster sidecars" warning naming QMAP_AXIS_PRIMARY.
 *   Scan fallback (no env, one roster) still rebuilds — never regresses.
 *   Env set but sidecar missing → warn + fall back to the scan.
 *
 * Behavioral assertions only (live-handle alias in-process, per the F4
 * contract); stderr is fd-captured, never shelled out. Cleanup closes the
 * primary handles first (§4: then the exit destructor leaves nothing),
 * so the test leaves no files behind.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <ttypt/qmap.h>
#include <ttypt/rec.h>
#include "stoma/stoma.h"

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
	return qmap_get(hd, row) != NULL;
}

static int text_finds(stoma_db_t *db, const char *tok, rec_ref_t ref)
{
	unsigned hd = qmap_open(NULL, NULL, QM_STR, QM_STR, 0xFF, 0);
	int handled = 0;
	char rid[24];
	uint32_t n;
	int found = 0;

	if (!hd)
		return 0;
	snprintf(rid, sizeof(rid), "%llu", (unsigned long long)ref);
	n = stoma_query(db, STOMA_AXIS_TEXT_FIELD, tok, hd, &handled);
	found = handled == 1 && n >= 1 && hd_has(hd, rid);
	qmap_close(hd);
	return found;
}

static int text_misses(stoma_db_t *db, const char *tok, rec_ref_t ref)
{
	unsigned hd = qmap_open(NULL, NULL, QM_STR, QM_STR, 0xFF, 0);
	int handled = 0;
	char rid[24];
	int miss = 0;

	if (!hd)
		return 0;
	snprintf(rid, sizeof(rid), "%llu", (unsigned long long)ref);
	(void)stoma_query(db, STOMA_AXIS_TEXT_FIELD, tok, hd, &handled);
	miss = handled == 1 && !hd_has(hd, rid);
	qmap_close(hd);
	return miss;
}

/* stderr fd-capture (no shellout): cap_begin redirects stderr to a temp
 * file; cap_end restores and returns its malloc'd contents (or NULL). */
static int saved_err = -1;
static char cap_path[PATH_MAX];

static void cap_begin(const char *path)
{
	int fd;

	fflush(stderr);
	if (saved_err < 0)
		saved_err = dup(fileno(stderr));
	snprintf(cap_path, sizeof(cap_path), "%s", path);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0) {
		(void)dup2(fd, fileno(stderr));
		(void)close(fd);
	}
}

static char *cap_end(void)
{
	FILE *f;
	long sz;
	char *buf = NULL;

	fflush(stderr);
	if (saved_err >= 0) {
		(void)dup2(saved_err, fileno(stderr));
		(void)close(saved_err);
		saved_err = -1;
	}
	f = fopen(cap_path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	sz = ftell(f);
	(void)fseek(f, 0, SEEK_SET);
	if (sz >= 0) {
		buf = malloc((size_t)sz + 1);
		if (buf) {
			size_t n = fread(buf, 1, (size_t)sz, f);
			buf[n] = '\0';
		}
	}
	fclose(f);
	return buf;
}

/* Seed one primary record + an (existence-only) roster sidecar. Returns
 * the still-open primary handle (left open: §4 forbids closing long-lived
 * handles; tempdir cleanup is the Makefile test target's job). */
static uint32_t seed_primary(const char *dir, const char *base,
		rec_ref_t ref, const char *text)
{
	char path[PATH_MAX], rsp[PATH_MAX + 16];
	FILE *f;
	uint32_t hd;

	snprintf(path, sizeof(path), "%s/%s", dir, base);
	hd = qmap_open(path, "hd", QM_HNDL, QM_STR, 4095,
			QM_AINDEX | QM_MIRROR);
	if (hd && text)
		qmap_put(hd, &ref, text);
	snprintf(rsp, sizeof(rsp), "%s.roster", path);
	f = fopen(rsp, "w");
	if (f) {
		fputs("v\n1\naxes\nstoma\n", f);
		fclose(f);
	}
	return hd;
}

int main(void)
{
	char d1[] = "/tmp/stoma-rebuild-test-XXXXXX";
	char d2[] = "/tmp/stoma-rebuild-test-XXXXXX";
	char spec[PATH_MAX], logf[PATH_MAX], prim[PATH_MAX];
	char *log;
	void *dbctx;
	uint32_t ha = 0, hb = 0, hc = 0;

	if (!mkdtemp(d1) || !mkdtemp(d2)) {
		printf("FAIL: mkdtemp (%s)\n", strerror(errno));
		return 1;
	}
	/* Deterministic mask (D11): the rebuild re-opens with this shape. */
	(void)setenv("QMAP_MASK", "4095", 1);

	ha = seed_primary(d1, "A.db", 1, "chirpy alpha harbour");
	hb = seed_primary(d1, "B.db", 1, "gamma beacon fjord");
	hc = seed_primary(d2, "C.db", 1, "delta kettle drum");
	CHECK(ha && hb && hc, "primaries-seeded");

	/* ── B-1: env primary wins with two rosters present ── */
	snprintf(prim, sizeof(prim), "%s/A.db", d1);
	(void)setenv("QMAP_AXIS_PRIMARY", prim, 1);
	snprintf(logf, sizeof(logf), "%s/cap.log", d1);
	snprintf(spec, sizeof(spec), "%s/A.db-stoma", d1);
	cap_begin(logf);
	dbctx = rec_axis_open(spec);
	log = cap_end();
	CHECK(dbctx != NULL, "b1-ctx");
	CHECK(dbctx && text_finds(dbctx, "alpha", 1), "b1-finds-own-text");
	CHECK(dbctx && text_misses(dbctx, "gamma", 1),
			"b1-no-sibling-leak");
	CHECK(log && strstr(log, "rebuilt 1 docs") != NULL,
			"b1-rebuilt-note");
	free(log);
	if (dbctx)
		stoma_close(dbctx);

	/* ── B-2: two rosters, no env → loud warn, valid empty index ── */
	(void)setenv("QMAP_AXIS_PRIMARY", "", 1);
	cap_begin(logf);
	dbctx = rec_axis_open(spec);
	log = cap_end();
	CHECK(dbctx != NULL, "b2-ctx-nonnull");
	CHECK(dbctx && text_misses(dbctx, "alpha", 1), "b2-alpha-empty");
	CHECK(dbctx && text_misses(dbctx, "gamma", 1), "b2-gamma-empty");
	CHECK(log && strstr(log, "2 roster sidecars") != NULL,
			"b2-count-warn");
	CHECK(log && strstr(log, "QMAP_AXIS_PRIMARY") != NULL,
			"b2-warn-names-env");
	free(log);
	if (dbctx)
		stoma_close(dbctx);

	/* ── scan fallback: one roster, no env → rebuild works ── */
	snprintf(spec, sizeof(spec), "%s/C.db-stoma", d2);
	cap_begin(logf);
	dbctx = rec_axis_open(spec);
	log = cap_end();
	CHECK(dbctx != NULL, "scan-ctx");
	CHECK(dbctx && text_finds(dbctx, "delta", 1),
			"scan-fallback-finds");
	CHECK(log && strstr(log, "rebuilt 1 docs") != NULL,
			"scan-fallback-note");
	free(log);
	if (dbctx)
		stoma_close(dbctx);

	/* ── env set but sidecar missing → warn + still fall back ── */
	snprintf(prim, sizeof(prim), "%s/nope.db", d2);
	(void)setenv("QMAP_AXIS_PRIMARY", prim, 1);
	cap_begin(logf);
	dbctx = rec_axis_open(spec);
	log = cap_end();
	CHECK(dbctx && text_finds(dbctx, "delta", 1),
			"missing-sidecar-still-rebuilds");
	CHECK(log && strstr(log, "falling back") != NULL,
			"missing-sidecar-warns");
	free(log);
	if (dbctx)
		stoma_close(dbctx);

	/* §4: long-lived handles stay open — the exit destructor saves them
	 * into the (still present) tempdirs, which the Makefile test target
	 * then removes. */
	(void)ha;
	(void)hb;
	(void)hc;
	printf("rebuilt-tests: %d/%d passed\n", total - failures, total);
	if (failures) {
		printf("rebuilt-tests: %d FAILURES\n", failures);
		return 1;
	}
	return 0;
}
