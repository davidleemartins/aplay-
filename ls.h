/* public domain Simple, Minimalistic, making list of files and directories
 *	©2017-2025 Yuichiro Nakada
 *	Modifications ©2026 David Lee Martins
 *
 * Basic usage:
 *	int num;
 *	LS_LIST *ls = ls_dir("dir/", LS_RECURSIVE|LS_RANDOM, &num);
 *
 * ls_dir returns FILES only (full paths), exactly `*num` of them, natural-order
 * sorted (or shuffled with LS_RANDOM). Directories are descended into with
 * LS_RECURSIVE but never appear as entries. Returns NULL with *num==0 when
 * there is nothing to list.
 * */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>

#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define LS_RECURSIVE	1
#define LS_RANDOM	2

typedef struct {
	char d_name[PATH_MAX];
} LS_LIST;

// Append all regular files under `dir` (descending into subdirs when
// LS_RECURSIVE) to the growing array. Single pass, no chdir. Returns 0 on
// success, -1 if `dir` could not be opened or memory ran out.
static int ls_collect(const char *dir, int flag, LS_LIST **arr, int *n, int *cap)
{
	DIR *dp = opendir(dir);
	if (!dp) return -1;

	struct dirent *entry;
	while ((entry = readdir(dp))) {
		if (!strcmp(".", entry->d_name) || !strcmp("..", entry->d_name)) continue;

		char path[PATH_MAX];
		if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name)
		    >= (int)sizeof(path)) continue;	// path too long — skip

		// Prefer d_type to avoid a stat per entry; fall back where unsupported.
		int isdir;
		if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_LNK) {
			isdir = (entry->d_type == DT_DIR);
		} else {
			struct stat st;
			if (stat(path, &st) != 0) continue;
			isdir = S_ISDIR(st.st_mode);
		}

		if (isdir) {
			if (flag & LS_RECURSIVE) ls_collect(path, flag, arr, n, cap);
			continue;	// directories are never entries
		}

		if (*n == *cap) {
			int ncap = *cap ? *cap * 2 : 64;
			LS_LIST *na = (LS_LIST*)realloc(*arr, ncap * sizeof(LS_LIST));
			if (!na) { closedir(dp); return -1; }
			*arr = na;
			*cap = ncap;
		}
		snprintf((*arr)[*n].d_name, PATH_MAX, "%s", path);
		(*n)++;
	}

	closedir(dp);
	return 0;
}

// Natural-order, case-insensitive compare: runs of digits compare by numeric
// value, so "2" sorts before "10" (e.g. track numbers) instead of lexically,
// and letters compare regardless of case.
static int ls_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int ls_natcmp(const char *a, const char *b)
{
	while (*a && *b) {
		int da = (*a >= '0' && *a <= '9');
		int db = (*b >= '0' && *b <= '9');
		if (da && db) {
			while (*a == '0') a++;	// skip leading zeros
			while (*b == '0') b++;
			const char *ae = a, *be = b;
			while (*ae >= '0' && *ae <= '9') ae++;
			while (*be >= '0' && *be <= '9') be++;
			int alen = ae - a, blen = be - b;
			if (alen != blen) return alen - blen;	// longer number is larger
			while (a < ae) {
				if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
				a++; b++;
			}
		} else {
			int ca = ls_lower((unsigned char)*a), cb = ls_lower((unsigned char)*b);
			if (ca != cb) return ca - cb;
			a++; b++;
		}
	}
	return ls_lower((unsigned char)*a) - ls_lower((unsigned char)*b);
}

int ls_comp_func(const void *a, const void *b)
{
	return ls_natcmp(((const LS_LIST*)a)->d_name, ((const LS_LIST*)b)->d_name);
}

// Fisher-Yates shuffle using /dev/urandom, falling back to rand().
static void ls_shuffle(LS_LIST *ls, int n)
{
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) srand(time(NULL));
	for (int i = n - 1; i > 0; i--) {
		uint32_t r;
		if (fd < 0 || read(fd, &r, sizeof(r)) != sizeof(r)) r = (uint32_t)rand();
		int a = r % (i + 1);
		LS_LIST tmp = ls[i];
		ls[i] = ls[a];
		ls[a] = tmp;
	}
	if (fd >= 0) close(fd);
}

LS_LIST *ls_dir(char *_dir, int flag, int *num)
{
	*num = 0;	// always define the out-param, even on the early returns below

	char dir[PATH_MAX];
	if (!realpath(_dir, dir)) snprintf(dir, sizeof(dir), "%s", _dir);

	int n = 0, cap = 0;
	LS_LIST *ls = NULL;
	if (ls_collect(dir, flag, &ls, &n, &cap) != 0 && n == 0) {
		fprintf(stderr, "cannot list %s\n", dir);
		free(ls);
		return NULL;
	}
	if (n == 0) {	// opened fine but nothing to play (empty / only subdirs)
		free(ls);
		return NULL;
	}

	if (flag & LS_RANDOM) ls_shuffle(ls, n);
	else                  qsort(ls, n, sizeof(LS_LIST), ls_comp_func);

	*num = n;
	return ls;
}

#include <ctype.h>
// Lowercased extension of the basename of `path` ("" when there is no dot).
// Returns a static buffer; extensions longer than 9 chars are truncated.
char *findExt(char *path)
{
	static char ext[10];
	ext[0] = 0;
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	const char *dot = strrchr(base, '.');
	if (!dot || !dot[1]) return ext;
	size_t i;
	for (i = 0; i < sizeof(ext) - 1 && dot[1 + i]; i++)
		ext[i] = (char)tolower((unsigned char)dot[1 + i]);
	ext[i] = 0;
	return ext;
}
