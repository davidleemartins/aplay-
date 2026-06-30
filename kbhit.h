/* public domain Simple, Minimalistic, kbhit function
 *	©2022 Yuichiro Nakada
 *	Modifications ©2026 David Lee Martins
 * */

#include <sys/ioctl.h>
#include <termios.h>

int kbhit()
{
	static const int STDIN = 0;
	static int initialized = 0;

	if (!initialized) {
		// Use termios to turn off line buffering
		struct termios term;
		tcgetattr(STDIN, &term);
		term.c_lflag &= ~ICANON;
		tcsetattr(STDIN, TCSANOW, &term);
		setbuf(stdin, NULL);
		initialized = 1;
	}

	int bytesWaiting;
	ioctl(STDIN, FIONREAD, &bytesWaiting);
	return bytesWaiting;
}

