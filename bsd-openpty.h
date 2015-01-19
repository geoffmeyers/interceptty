#include <termios.h>
#ifdef HAVE_PTY_H
# include <pty.h>
#endif

int my_openpty(int *, int *, char *);
#define TTYLEN          32
