/* public domain Simple, Minimalistic, kbhit function
 *	©2022 Yuichiro Nakada
 *	Modifications ©2026 David Lee Martins
 *
 * Pure poll: returns the number of bytes waiting on stdin. The terminal must
 * already be in non-canonical mode — main() sets it for direct playback (-n/-T)
 * and browser.h's br_term_playback() sets it for browser-launched playback.
 * (kbhit used to flip ICANON off itself, behind the terminal-restore logic's
 * back; that lazy mutation is gone.)
 * */

#include <sys/ioctl.h>
#include <termios.h>

int kbhit()
{
	static int initialized = 0;
	if (!initialized) {
		setbuf(stdin, NULL);	// unbuffered stdin so FIONREAD sees every byte
		initialized = 1;
	}

	int bytesWaiting = 0;	// stays 0 if FIONREAD fails (e.g. stdin not a tty)
	ioctl(0, FIONREAD, &bytesWaiting);
	return bytesWaiting;
}
