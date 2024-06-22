/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

struct termios orig_termios;

/*** util ***/

void die(const char *s) {
  perror(s); // looks at the `errno`
  exit(1);
}

/*** terminal ***/

/**
 * Turn off raw mode
 */
void disable_raw_mode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
    die("tcsetattr");
  }
}

/**
 * Turn off the ECHO feature of the terminal
 */
void enable_raw_mode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
    // invalid ioctl
    die("tcgetattr");
  }
  atexit(disable_raw_mode);

  struct termios raw = orig_termios;
  raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  // c_lflag - local modes
  // ECHO - a bitflag
  //   - 00000000000000000000000000001000 in binary
  raw.c_lflag &=
      ~(ECHO | ICANON | ISIG | IEXTEN); // turn off ECHO _and_ Canonical mode

  // `cc` - control characters, array of bytes that control terminal settings
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 30; // 1/10 of a second
  // TCSAFLUSH - how/when the changes take effect
  //  - waits for all pending output to be written to terminal
  //  - discards any input not read yet
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

/*** init ***/

int main() {
  enable_raw_mode();

  // read 1 byte from stdin into c until no more bytes to read
  // read() returns number of bytes read; returns 0 if reached EOF
  // use Ctrl-D to tell read() it has reached end of file
  while (1) {
    char c = '\0';
    // read() returns -1 on failure
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) {
      die("read");
    }
    if (iscntrl(c)) {
      // check if character is a control character
      // non-printable
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }
    if (c == 'q')
      break;
  }
  return 0;
}
