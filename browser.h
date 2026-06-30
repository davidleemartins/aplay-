/* Tier 1 pre-playback file browser for aplay+
 *	©2026 David Lee Martins — hand-rolled full-screen ANSI directory chooser, no ncurses.
 *
 * A ranger-like terminal chooser used BEFORE playback: navigate directories,
 * press Enter on a dir/file to hand off to the existing blocking play_dir/
 * play_file, then return to the browser when playback ends. The audio path is
 * untouched; this only wraps the dispatch.
 *
 * Depends (from aplay+.c, include this header AFTER they are defined):
 *	void play_dir(char *name, char *type, char *regexp, int flag, const char *start);
 *	int  select_alsa_device(char *out, size_t n);
 *	void config_save_device(const char *dev);
 *	char *dev;  (global current device)
 * */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

typedef struct {
	char name[256];	// basename only
	int is_dir;
} BR_ENTRY;

/* ---- key codes ---- */
#define BR_NONE	0
#define BR_UP	1000
#define BR_DOWN	1001
#define BR_LEFT	1002
#define BR_RIGHT	1003
#define BR_ENTER	1004
#define BR_QUIT	1005
#define BR_DEVICE	1006
#define BR_SHUFFLE	1007	/* x: shuffle current dir */
#define BR_SHUFFLE_REC	1008	/* X: shuffle current dir + subdirs */
#define BR_THEME	1009	/* t: pick a color theme */

static int br_is_playlist(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	return !strcasecmp(dot + 1, "m3u") || !strcasecmp(dot + 1, "m3u8");
}

/* ---- audio-file filter (mirror play_file's supported extensions) ---- */
static int br_is_audio(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot) return 0;
	const char *e = dot + 1;
	return !strcasecmp(e, "flac") || !strcasecmp(e, "mp3") ||
	       !strcasecmp(e, "mp4")  || !strcasecmp(e, "m4a") ||
	       !strcasecmp(e, "ogg")  || !strcasecmp(e, "wav") ||
	       !strcasecmp(e, "wma")  || !strcasecmp(e, "dsf") ||
	       br_is_playlist(name);
}

/* ---- per-directory model (non-recursive; dirs first, then files) ---- */
static int br_cmp(const void *a, const void *b)
{
	const BR_ENTRY *ea = a, *eb = b;
	if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
	return ls_natcmp(ea->name, eb->name);
}

static BR_ENTRY *br_read(const char *path, int *count)
{
	DIR *dp = opendir(path);
	if (!dp) { *count = 0; return NULL; }

	int cap = 64, n = 0;
	BR_ENTRY *e = malloc(cap * sizeof(BR_ENTRY));
	struct dirent *d;
	while ((d = readdir(dp))) {
		if (d->d_name[0] == '.') continue;	// skip ., .. and dotfiles

		char full[PATH_MAX];
		snprintf(full, sizeof full, "%s/%s", path, d->d_name);
		struct stat st;
		int isdir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);

		if (!isdir && !br_is_audio(d->d_name)) continue;	// only dirs + audio

		if (n == cap) { cap *= 2; e = realloc(e, cap * sizeof(BR_ENTRY)); }
		snprintf(e[n].name, sizeof e[n].name, "%s", d->d_name);
		e[n].is_dir = isdir;
		n++;
	}
	closedir(dp);
	qsort(e, n, sizeof(BR_ENTRY), br_cmp);
	*count = n;
	return e;
}

static int br_find(BR_ENTRY *e, int n, const char *name)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(e[i].name, name)) return i;
	return 0;
}

static void br_join(const char *dir, const char *name, char *out)
{
	if (!strcmp(dir, "/")) snprintf(out, PATH_MAX, "/%s", name);
	else                   snprintf(out, PATH_MAX, "%s/%s", dir, name);
}

/* parent path of `path`, plus the basename we came from (for cursor restore) */
static void br_parent(const char *path, char *parent, char *base)
{
	const char *slash = strrchr(path, '/');
	if (slash && slash[1]) snprintf(base, PATH_MAX, "%s", slash + 1);
	else base[0] = 0;

	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s/..", path);
	if (!realpath(tmp, parent)) snprintf(parent, PATH_MAX, "%s", path);
}

/* ---- terminal raw mode (independent of kbhit.h, blocking single-char) ---- */
static struct termios br_orig;
static volatile sig_atomic_t br_resized;
static volatile sig_atomic_t g_alt_active = 0;	/* true while the alt-screen UI is shown */

static void br_winch(int s) { (void)s; br_resized = 1; }

// Apply the browser's raw mode. br_orig must already hold the TRUE original
// terminal state (captured once in browse()) — do NOT re-read it here, or a call
// made while playback's echo-off mode is active would corrupt the saved state
// and leave the terminal without echo after quit.
static void br_term_init(void)
{
	struct termios t = br_orig;
	t.c_lflag &= ~(ICANON | ECHO | ISIG);
	t.c_iflag &= ~(IXON | ICRNL);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &t);
}

static void br_term_restore(void) { tcsetattr(0, TCSANOW, &br_orig); }

// Terminal mode for handing off to the legacy blocking player: non-canonical and
// no-echo so the player's kbhit()/getchar() keys (p/n/space/q) register
// immediately on every track — kbhit() only disables ICANON once itself, so we
// must guarantee it here for repeated playbacks. ISIG stays on so Ctrl-C works.
static void br_term_playback(void)
{
	struct termios t = br_orig;
	t.c_lflag &= ~(ICANON | ECHO);
	t.c_lflag |= ISIG;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &t);
}

static void br_enter_screen(void) { g_alt_active = 1; (void)!write(1, "\e[?1049h\e[?25l\e[2J", 18); }
static void br_leave_screen(void) { g_alt_active = 0; (void)!write(1, "\e[?1049l\e[?25h", 14); }

static void br_term_size(int *rows, int *cols)
{
	struct winsize ws;
	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	} else {
		*rows = 24;
		*cols = 80;
	}
}

/* read one more byte with a 0.1s timeout (for decoding ESC sequences) */
static int br_readtimed(unsigned char *c)
{
	struct termios t;
	tcgetattr(0, &t);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 1;
	tcsetattr(0, TCSANOW, &t);
	int n = read(0, c, 1);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &t);
	return n;
}

static int br_getkey(void)
{
	unsigned char c;
	int n = read(0, &c, 1);
	if (n <= 0) return BR_NONE;	// EINTR (SIGWINCH) etc.

	switch (c) {
	case '\r': case '\n': return BR_ENTER;
	case 'q':             return BR_QUIT;
	case 'd':             return BR_DEVICE;
	case 'x':             return BR_SHUFFLE;
	case 'X':             return BR_SHUFFLE_REC;
	case 't':             return BR_THEME;
	case 'k':             return BR_UP;
	case 'j':             return BR_DOWN;
	case 'h':             return BR_LEFT;
	case 'l':             return BR_RIGHT;
	case 127: case 8:     return BR_LEFT;	// Backspace = parent
	case 27: {				// ESC or escape sequence
		unsigned char a, b;
		if (br_readtimed(&a) <= 0) return BR_QUIT;	// lone ESC
		if (a == '[' || a == 'O') {
			if (br_readtimed(&b) <= 0) return BR_NONE;
			switch (b) {
			case 'A': return BR_UP;
			case 'B': return BR_DOWN;
			case 'C': return BR_RIGHT;
			case 'D': return BR_LEFT;
			}
		}
		return BR_NONE;
	}
	}
	return BR_NONE;
}

/* ---- render: one buffered write to avoid flicker ---- */
static void br_render(const char *cwd, BR_ENTRY *e, int count,
                      int cursor, int top, int rows, int cols)
{
	if (cols > 1024) cols = 1024;
	if (cols < 8) cols = 8;
	int height = rows - 2;
	if (height < 1) height = 1;

	size_t cap = (size_t)(rows + 2) * (cols * 2 + 64) + 256;
	char *buf = malloc(cap);
	size_t p = 0;
#define APP(...) do { int _w = snprintf(buf + p, cap - p, __VA_ARGS__); \
                      if (_w > 0) p += (size_t)_w; } while (0)

	APP("\e[H");
	/* header: current path */
	APP("%s %.*s \e[0m\e[K\r\n", theme.header, cols - 2, cwd);

	/* list */
	for (int r = 0; r < height; r++) {
		int idx = top + r;
		if (idx < count) {
			int sel = (idx == cursor);
			if (sel) APP("%s", theme.sel);
			if (e[idx].is_dir) {
				if (!sel) APP("%s", theme.dir);
				APP(" %.*s/", cols - 3, e[idx].name);
			} else {
				if (!sel) APP("%s", theme.file);
				APP(" %.*s", cols - 2, e[idx].name);
			}
			APP("\e[0m\e[K\r\n");
		} else if (count == 0 && r == 0) {
			APP(" (empty \xe2\x80\x94 \xe2\x86\x90 to go up)\e[K\r\n");
		} else {
			APP("\e[K\r\n");
		}
	}

	/* footer: key help */
	APP("\e[%d;1H%s %.*s \e[0m\e[K", rows, theme.footer, cols - 2,
	    "\xe2\x86\x91\xe2\x86\x93 move  \xe2\x86\x92 open  \xe2\x86\x90 up  \xe2\x8f\x8e play  x shuffle  X shuffle+subdirs  t theme  d device  q quit");

	(void)!write(1, buf, p);
	free(buf);
#undef APP
}

// Ensure the saved playback device is actually present before playing. If it's
// gone (e.g. a USB DAC was unplugged), drop to the device picker and let the user
// choose one. Returns 1 if it's safe to play, 0 if cancelled / still unavailable.
static int br_ensure_device(void)
{
	if (device_available(dev)) return 1;

	br_leave_screen();
	br_term_restore();
	fprintf(stderr, "\nDevice '%s' is unavailable (unplugged or busy).\n", dev);
	static char buf[64];
	int ok = 0;
	if (select_alsa_device(buf, sizeof buf)) {
		config_save_device(buf);
		dev = buf;
		ok = device_available(dev);
	}
	br_term_init();
	br_enter_screen();
	return ok;
}

/* ---- main browser loop ---- */
static void browse(char *start, const char *focus, char *type, char *regexp, int flag)
{
	/* validate / fall back the starting directory */
	struct stat st;
	if (stat(start, &st) != 0 || !S_ISDIR(st.st_mode)) {
		char *h = getenv("HOME");
		start = h ? h : "/";
	}

	char cwd[PATH_MAX];
	if (!realpath(start, cwd)) snprintf(cwd, sizeof cwd, "%s", start);

	int count = 0;
	BR_ENTRY *ents = br_read(cwd, &count);
	int cursor = focus ? br_find(ents, count, focus) : 0;
	int top = 0;

	tcgetattr(0, &br_orig);	/* capture the TRUE original terminal state once */
	br_term_init();
	br_enter_screen();
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = br_winch;	/* no SA_RESTART: read() gets EINTR on resize */
	sigaction(SIGWINCH, &sa, NULL);

	int running = 1;
	while (running) {
		int rows, cols;
		br_term_size(&rows, &cols);
		int height = rows - 2;
		if (height < 1) height = 1;

		if (cursor < 0) cursor = 0;
		if (cursor > count - 1) cursor = count - 1;
		if (cursor < 0) cursor = 0;
		if (cursor < top) top = cursor;
		if (cursor >= top + height) top = cursor - height + 1;
		if (top < 0) top = 0;

		br_render(cwd, ents, count, cursor, top, rows, cols);

		int key = br_getkey();
		switch (key) {
		case BR_UP:
			if (cursor > 0) cursor--;
			break;
		case BR_DOWN:
			if (cursor < count - 1) cursor++;
			break;
		case BR_LEFT: {
			char parent[PATH_MAX], base[PATH_MAX];
			br_parent(cwd, parent, base);
			if (strcmp(parent, cwd)) {
				free(ents);
				snprintf(cwd, sizeof cwd, "%s", parent);
				ents = br_read(cwd, &count);
				cursor = br_find(ents, count, base);
				top = 0;
			}
			break;
		}
		case BR_RIGHT:
			if (count > 0 && ents[cursor].is_dir) {
				char nd[PATH_MAX];
				br_join(cwd, ents[cursor].name, nd);
				free(ents);
				snprintf(cwd, sizeof cwd, "%s", nd);
				ents = br_read(cwd, &count);
				cursor = 0;
				top = 0;
			}
			break;
		case BR_ENTER:
			if (count > 0) {
				if (!br_ensure_device()) break;	// device gone & not re-picked
				char target[PATH_MAX];
				br_join(cwd, ents[cursor].name, target);
				int isdir = ents[cursor].is_dir;

				/* hand off to the existing blocking player on a clean screen */
				br_leave_screen();
				br_term_playback();
				(void)!write(1, "\e[H\e[2J", 7);
				if (isdir) {
					play_dir(target, type, regexp, flag, NULL);
				} else if (br_is_playlist(ents[cursor].name)) {
					play_m3u(target, flag);
				} else {
					/* play the album (current dir) starting at this track,
					 * so prev/next navigate the rest of the directory */
					play_dir(cwd, type, regexp, flag, ents[cursor].name);
				}
				br_term_init();
				br_enter_screen();
			}
			break;
		case BR_SHUFFLE:
		case BR_SHUFFLE_REC: {
			/* shuffle-play the current directory (optionally recursively) */
			if (!br_ensure_device()) break;	// device gone & not re-picked
			int f = flag | LS_RANDOM;
			if (key == BR_SHUFFLE_REC) f |= LS_RECURSIVE;
			br_leave_screen();
			br_term_playback();
			(void)!write(1, "\e[H\e[2J", 7);
			play_dir(cwd, type, regexp, f, NULL);
			br_term_init();
			br_enter_screen();
			break;
		}
		case BR_DEVICE: {
			/* re-open the device picker in normal mode, then resume */
			static char br_devbuf[64];
			br_leave_screen();
			br_term_restore();
			if (select_alsa_device(br_devbuf, sizeof br_devbuf)) {
				config_save_device(br_devbuf);
				dev = br_devbuf;
			}
			br_term_init();
			br_enter_screen();
			break;
		}
		case BR_THEME: {
			/* pick a color theme, apply it live, and save it */
			static char br_thbuf[24];
			br_leave_screen();
			br_term_restore();
			if (select_theme(br_thbuf, sizeof br_thbuf)) {
				theme_preset(br_thbuf);
				config_save_theme(br_thbuf);
			}
			br_term_init();
			br_enter_screen();
			break;
		}
		case BR_QUIT: {
			/* confirm before leaving */
			char prompt[160];
			snprintf(prompt, sizeof prompt,
			         "\e[%d;1H%s Quit aplay+?  y = yes,  any other key = no \e[0m\e[K",
			         rows, theme.footer);
			(void)!write(1, prompt, strlen(prompt));
			unsigned char ans = 0;
			(void)!read(0, &ans, 1);
			if (ans == 'y' || ans == 'Y') running = 0;
			break;
		}
		}
	}

	free(ents);
	br_leave_screen();
	br_term_restore();
}
