/* Unit tests for ls.h: ls_natcmp, findExt, ls_dir.
 * ©2026 David Lee Martins
 *
 * ls_dir contract asserted here (post-2026-07-03 rewrite):
 *   - returns FILES only (full paths), *num == exact file count
 *   - no empty/padding slots
 *   - LS_RECURSIVE descends into subdirs, plain call does not
 *   - sorted natural-order unless LS_RANDOM; LS_RANDOM is a permutation
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "../random.h"
#include "../ls.h"

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void touch(const char *path)
{
	FILE *f = fopen(path, "w");
	if (f) { fputs("x", f); fclose(f); }
}

/* does the list contain a path whose basename matches? */
static int has_base(LS_LIST *ls, int num, const char *base)
{
	for (int i = 0; i < num; i++) {
		const char *s = strrchr(ls[i].d_name, '/');
		s = s ? s + 1 : ls[i].d_name;
		if (!strcmp(s, base)) return 1;
	}
	return 0;
}

static void test_natcmp(void)
{
	CHECK(ls_natcmp("2", "10") < 0, "2 should sort before 10");
	CHECK(ls_natcmp("10", "2") > 0, "10 should sort after 2");
	CHECK(ls_natcmp("track2", "Track10") < 0, "track2 < Track10 (case-insensitive)");
	CHECK(ls_natcmp("a", "a") == 0, "identical strings equal");
	CHECK(ls_natcmp("A", "a") == 0, "case-insensitive equality");
	CHECK(ls_natcmp("02", "2") == 0, "leading zeros compare equal");
	CHECK(ls_natcmp("1 - x", "10 - y") < 0, "1-prefix < 10-prefix");
	CHECK(ls_natcmp("", "") == 0, "empty strings equal");
	CHECK(ls_natcmp("", "a") < 0, "empty < non-empty");
	CHECK(ls_natcmp("abc", "abd") < 0, "plain lexical order");
	CHECK(ls_natcmp("9", "10") < 0, "9 < 10 numerically");
	CHECK(ls_natcmp("100", "20") > 0, "100 > 20 numerically");
}

static void test_findext(void)
{
	CHECK(!strcmp(findExt("foo.flac"), "flac"), "simple extension");
	CHECK(!strcmp(findExt("FOO.FLAC"), "flac"), "extension lowercased");
	CHECK(!strcmp(findExt("a/b.c/track.mp3"), "mp3"), "extension after path");
	CHECK(!strcmp(findExt("noext"), ""), "no dot -> empty (or at most 9 trailing chars)");
	CHECK(!strcmp(findExt(""), ""), "empty string must not crash");
	CHECK(!strcmp(findExt("x"), ""), "1-char no-dot string");
	CHECK(!strcmp(findExt("a.m3u8"), "m3u8"), "m3u8");
}

static void test_ls_dir(const char *root)
{
	char top[PATH_MAX];
	int num = 0;

	/* --- plain (non-recursive): 3 files at top level, natural order --- */
	snprintf(top, sizeof top, "%s/album", root);
	LS_LIST *ls = ls_dir(top, 0, &num);
	CHECK(ls != NULL, "ls_dir returned NULL for populated dir");
	if (ls) {
		CHECK(num == 3, "top-level file count: want 3, got %d", num);
		for (int i = 0; i < num; i++)
			CHECK(ls[i].d_name[0] != 0, "slot %d is empty", i);
		CHECK(has_base(ls, num, "1 - one.flac") &&
		      has_base(ls, num, "2 - two.flac") &&
		      has_base(ls, num, "10 - ten.flac"), "all top files present");
		CHECK(!has_base(ls, num, "deep.flac"), "non-recursive must not descend");
		/* natural sort: 1, 2, 10 */
		if (num == 3) {
			CHECK(strstr(ls[0].d_name, "1 - one") != NULL, "order[0]=%s", ls[0].d_name);
			CHECK(strstr(ls[1].d_name, "2 - two") != NULL, "order[1]=%s", ls[1].d_name);
			CHECK(strstr(ls[2].d_name, "10 - ten") != NULL, "order[2]=%s", ls[2].d_name);
		}
		free(ls);
	}

	/* --- recursive: + 2 files in subdir --- */
	ls = ls_dir(top, LS_RECURSIVE, &num);
	CHECK(ls != NULL, "recursive ls_dir returned NULL");
	if (ls) {
		CHECK(num == 5, "recursive file count: want 5, got %d", num);
		CHECK(has_base(ls, num, "deep.flac") && has_base(ls, num, "deeper.flac"),
		      "recursive listing includes subdir files");
		for (int i = 0; i < num; i++)
			CHECK(ls[i].d_name[0] != 0, "recursive slot %d empty", i);
		free(ls);
	}

	/* --- shuffle: same set, no empties --- */
	ls = ls_dir(top, LS_RECURSIVE | LS_RANDOM, &num);
	CHECK(ls != NULL, "shuffled ls_dir returned NULL");
	if (ls) {
		CHECK(num == 5, "shuffled count: want 5, got %d", num);
		CHECK(has_base(ls, num, "1 - one.flac") && has_base(ls, num, "2 - two.flac") &&
		      has_base(ls, num, "10 - ten.flac") && has_base(ls, num, "deep.flac") &&
		      has_base(ls, num, "deeper.flac"), "shuffle must be a permutation");
		for (int i = 0; i < num; i++)
			CHECK(ls[i].d_name[0] != 0, "shuffled slot %d empty", i);
		free(ls);
	}

	/* --- dir containing only subdirs: valid empty listing, num set --- */
	snprintf(top, sizeof top, "%s/onlydirs", root);
	num = -777;
	ls = ls_dir(top, 0, &num);
	CHECK(num == 0, "all-subdirs dir: want num=0, got %d", num);
	free(ls);

	/* --- empty dir --- */
	snprintf(top, sizeof top, "%s/empty", root);
	num = -777;
	ls = ls_dir(top, 0, &num);
	CHECK(num == 0, "empty dir: want num=0, got %d", num);
	free(ls);

	/* --- missing dir: num must still be defined --- */
	snprintf(top, sizeof top, "%s/no-such-dir", root);
	num = -777;
	ls = ls_dir(top, 0, &num);
	CHECK(num == 0, "missing dir: want num=0, got %d", num);
	CHECK(ls == NULL || num == 0, "missing dir must not fabricate entries");
	free(ls);
}

int main(void)
{
	/* build fixture tree in a temp dir */
	char root[] = "/tmp/aplay-test-ls-XXXXXX";
	if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/album", root);            mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/album/sub", root);        mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/onlydirs", root);         mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/onlydirs/a", root);       mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/onlydirs/b", root);       mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/empty", root);            mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/album/1 - one.flac", root);   touch(p);
	snprintf(p, sizeof p, "%s/album/2 - two.flac", root);   touch(p);
	snprintf(p, sizeof p, "%s/album/10 - ten.flac", root);  touch(p);
	snprintf(p, sizeof p, "%s/album/sub/deep.flac", root);  touch(p);
	snprintf(p, sizeof p, "%s/album/sub/deeper.flac", root); touch(p);

	test_natcmp();
	test_findext();
	test_ls_dir(root);

	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
	(void)!system(cmd);

	printf("test_ls: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
