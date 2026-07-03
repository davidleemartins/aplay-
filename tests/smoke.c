/* Decode smoke test: play each file given on the command line through the real
 * play_file() pipeline to the ALSA "null" device (silent sink). Links against
 * aplay+.c compiled with -Dmain=aplay_main. Run with stdin from an idle pipe
 * so key()'s FIONREAD poll sees no input.
 * ©2026 David Lee Martins
 */
#include <stdio.h>
#include <string.h>

extern char *dev;
extern int play_file(char *path, int format, int flag);
extern int crosstalk;
#define USE_CROSSTALK 256	/* keep in sync with aplay+.c */

int main(int argc, char **argv)
{
	dev = "null";
	int flag = 0, start = 1;
	if (argc > 1 && !strcmp(argv[1], "-c")) {	/* exercise the crosstalk path */
		flag |= USE_CROSSTALK;
		crosstalk = 1;
		start = 2;
	}
	int bad = 0;
	for (int i = start; i < argc; i++) {
		printf("== %s\n", argv[i]);
		if (!play_file(argv[i], 0, flag)) {
			fprintf(stderr, "unsupported: %s\n", argv[i]);
			bad = 1;
		}
		printf("\n");
	}
	return bad;
}
