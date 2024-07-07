/*** includes ***/

#include <time.h>
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

/**
 * 0x0001ffff
 * sets the upper 3 bits of the character to 0
 * similar to what ctrl does - strips bits 5 and 6 from input and sends to
 * terminal
 */
#define CTRL_KEY(k) ((k) & 0x1f)

enum EDITOR_KEY {
    ARROW_LEFT = 1000,  // high enough, outside range of char
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

/*** data ***/

struct editor_row {
    int size;
    int rsize;    // size of render
    char *chars;  // dynamically allocated
    char *render;
};

// Global state of the editor
struct editor_config {
    int cx, cy;
    int rx;      // index into the `render` field
    int rowoff;  // what row of file the user is currently scrolled to
    int coloff;  // horizontal scrolling
    int screen_rows;
    int screen_cols;
    int num_rows;
    char *filename;
    char status_msg[80];
    time_t status_msg_time;
    struct editor_row *rows;
    struct termios orig_termios;
};
struct editor_config E;

/*** util ***/

void die(const char *s) {
    // clear the screen and reposition the cursor on exit
    int x = write(STDOUT_FILENO, "\x1b[2J", 4);
    int y = write(STDOUT_FILENO, "\x1b[H", 3);
    if (x == -1 || y == -1) {
        exit(1);
    }
    perror(s);  // looks at the `errno`
    exit(1);
}

/*** terminal ***/

/**
 * Turn off raw mode
 */
void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

/**
 * Turn off the ECHO feature of the terminal
 */
void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        // invalid ioctl
        die("tcgetattr");
    }
    atexit(disable_raw_mode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    // c_lflag - local modes
    // ECHO - a bitflag
    //   - 00000000000000000000000000001000 in binary
    raw.c_lflag &=
        ~(ECHO | ICANON | ISIG | IEXTEN);  // turn off ECHO _and_ Canonical mode

    // `cc` - control characters, array of bytes that control terminal settings
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;  // 1/10 of a second
    // TCSAFLUSH - how/when the changes take effect
    //  - waits for all pending output to be written to terminal
    //  - discards any input not read yet
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

/**
 * Wait for one keypress, then return it
 */
int editor_read_key() {
    int n_read;
    char c;
    // read() returns -1 on failure
    while ((n_read = read(STDIN_FILENO, &c, 1)) != 1) {
        if (n_read == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    // Map arrow keys to j/h/k/l
    // arrow key presses are interpreted as an escape sequence
    // `'\x1b'`, `[`, followed by
    //   - `'A'`
    //   - `'B'`
    //   - `'C'`
    //   - `'D'`
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME_KEY;
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int get_cursor_position(int *rows, int *cols) {
    // the `n` command - query terminal for status info
    // response should be CPR (cursor position report)
    //   - `Esc [ Pn;Pn R`

    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        ++i;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') {
        return -1;
    }
    // skipping '\x1b' and '['
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        // parse two integers separated by ';'
        return -1;
    }

    return 0;
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // 1. move cursor to bottom right corner
        // 2. get cursor position
        // the `C` command - moves cursor to the right
        // the `B` command - moves cursor down
        // do _NOT_ use the `H` command like `999;999H` because it does not
        // specify what happens if moving pass screen edge
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

/**
 * Convert a `chars` index into a `render` index
 */
int editor_row_cx_to_rx(struct editor_row *row, int cx) {
    // loop through all characters to the left of `cx`
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

void editor_update_row(struct editor_row *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }
    free(row->render);
    // each tab is 8 chars; initial row->size already has one char for each tab
    // including '\0'
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';  // advance the cursor at least one space
            while (idx % KILO_TAB_STOP != 0) {
                // until running into tabstop
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editor_append_row(char *s, size_t len) {
    E.rows = realloc(E.rows, sizeof(struct editor_row) * (E.num_rows + 1));
    int at = E.num_rows;
    E.rows[at].size = len;
    E.rows[at].chars = malloc(len + 1);
    memcpy(E.rows[at].chars, s, len);
    E.rows[at].chars[len] = '\0';

    E.rows[at].rsize = 0;
    E.rows[at].render = NULL;

    editor_update_row(&E.rows[at]);

    E.num_rows++;
}

/*** file I/O ***/

void editor_open(char *filename) {
    free(E.filename);
    // assuming you will free this memory yourself
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }
    char *line = NULL;
    size_t line_cap = 0;  // line capacity
    // ssize_t - signed size_t; can store at least -1 to 2^15 - 1
    ssize_t line_len;  // excluding '\0'
    while ((line_len = getline(&line, &line_cap, fp)) != -1) {
        while (line_len > 0 &&
               (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line_len--;  // strips off the newline
        }
        editor_append_row(line, line_len);
    }

    if (line) free(line);
    fclose(fp);
}

/*** append buffer ***/

/**
 * append buffer
 */
struct abuf {
    char *buffer;
    int len;
};

#define ABUF_INIT \
    { NULL, 0 }

void abuf_append(struct abuf *ab, const char *s, int len) {
    // realloc either extend the size of the current block of memory
    // or _frees_ the current block of memory and allocates new block
    char *new = realloc(ab->buffer, ab->len + len);
    if (!new) return;
    memcpy(&new[ab->len], s, len);
    ab->buffer = new;
    ab->len += len;
}

void abuf_free(struct abuf *ab) { free(ab->buffer); }

/*** input ***/

/**
 * Move cursors using J/H/K/L
 */
void editor_move_cursor(int key) {
    struct editor_row *curr_row = (E.cy >= E.num_rows) ? NULL : &E.rows[E.cy];

    switch (key) {
        case ARROW_DOWN:
            // allowing pass one line of the file
            if (E.cy < E.num_rows) {
                E.cy++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                // pressing <- allows user to move to the end of previous line
                E.cy--;
                E.cx = E.rows[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (curr_row && E.cx < curr_row->size) {
                E.cx++;
            } else if (curr_row && E.cx == curr_row->size) {
                // allow pressing -> to move to the beginning of next line
                E.cy++;
                E.cx = 0;
            }
            break;
    }

    // prevent cursor from moving pass the end of a line
    curr_row = (E.cy >= E.num_rows) ? NULL : &E.rows[E.cy];
    int curr_row_len = curr_row ? curr_row->size : 0;
    if (E.cx > curr_row_len) {
        E.cx = curr_row_len;
    }
}

/**
 * Wait for one keypress, then _handle_ it
 */
void editor_process_keypress() {
    int c = editor_read_key();

    switch (c) {
        case CTRL_KEY('q'):
            // enables <Ctrl-q> to quit
            if (write(STDOUT_FILENO, "\x1b[2J", 4) == -1) {
                die("write");
            }
            if (write(STDOUT_FILENO, "\x1b[H", 3) == -1) {
                die("write");
            }
            exit(0);
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            // move to the end of the line
            if (E.cy < E.num_rows) {
                E.cx = E.rows[E.cy].size;
            }
            break;
        case PAGE_UP:
        case PAGE_DOWN: {
            // first, place cursor at top/bottom of the screen
            // then, simulate an entire screen worth of up/down
            if (c == PAGE_UP) {
                E.cy = E.rowoff;
            } else if (c == PAGE_DOWN) {
                E.cy = E.rowoff + E.screen_rows - 1;
                if (E.cy > E.num_rows) {
                    E.cy = E.num_rows;
                }
            }
            int times = E.screen_rows;
            while (times--) {
                editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        } break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_RIGHT:
        case ARROW_LEFT:
            editor_move_cursor(c);
            break;
    }
}

/*** output ***/

void editor_scroll() {
    E.rx = 0;
    if (E.cy < E.num_rows) {
        E.rx = editor_row_cx_to_rx(&E.rows[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        // check if cursor is _above_ the visible window
        E.rowoff = E.cy;  // scroll _up_ to where the cursor is
    }

    if (E.cy >= E.rowoff + E.screen_rows) {
        // if cursor is past the bottom of visible screen
        E.rowoff = E.cy - E.screen_rows + 1;
    }

    if (E.rx < E.coloff) {
        // need to scroll to the left
        E.coloff = E.rx;
    }

    if (E.rx >= E.coloff + E.screen_cols) {
        // cursor past the right edge of screen
        E.coloff = E.rx - E.screen_cols + 1;
    }
}

void editor_draw_rows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screen_rows; ++y) {
        int file_row = y + E.rowoff;
        if (file_row >= E.num_rows) {
            if (E.num_rows == 0 && y == E.screen_rows / 3) {
                // _ONLY_ display the welcome message if there is no content
                // read from a file
                char welcome[80];
                int welcome_len =
                    snprintf(welcome, sizeof(welcome),
                             "Kilo Editor -- version %s", KILO_VERSION);
                // truncate the welcome message if the screen is too narrow
                if (welcome_len > E.screen_cols) welcome_len = E.screen_cols;

                int padding = (E.screen_cols - welcome_len) / 2;
                if (padding) {
                    abuf_append(ab, "~", 1);
                    padding--;
                }
                while (padding--) {
                    abuf_append(ab, " ", 1);
                }
                abuf_append(ab, welcome, welcome_len);
            } else {
                abuf_append(ab, "~", 1);
            }

        } else {
            // draw content read from file
            int len = E.rows[file_row].rsize - E.coloff;
            if (len < 0) {
                len = 0;
            }
            if (len > E.screen_cols) {
                len = E.screen_cols;  // truncate the line, only display until
                                      // edge of the screen
            }
            abuf_append(ab, &E.rows[file_row].render[E.coloff], len);
        }
        // erase current line, from the active cursor position to the end of
        // line
        abuf_append(ab, "\x1b[K", 3);
        abuf_append(ab, "\r\n", 2);
    }
}

void editor_draw_status_bar(struct abuf *ab) {
    abuf_append(ab, "\x1b[7m", 4);  // switch to inverted colors

    char status[80];
    char rstatus[80];  // current line number
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                       E.filename ? E.filename : "[No Name]", E.num_rows);
    int rlen =
        snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.num_rows);
    if (len > E.screen_cols) {
        len = E.screen_cols;
    }
    abuf_append(ab, status, len);
    while (len < E.screen_cols) {
        if (E.screen_cols - len == rlen) {
            // align to the right edge of the screen
            abuf_append(ab, rstatus, rlen);
            break;
        } else {
            abuf_append(ab, " ", 1);
            len++;
        }
    }
    abuf_append(ab, "\x1b[m", 3);  // switch back to normal formatting
    abuf_append(ab, "\r\n", 2);    // message & timestamp
}

void editor_draw_message_bar(struct abuf *ab) {
    abuf_append(ab, "\x1b[K", 3);
    int msg_len = strlen(E.status_msg);
    if (msg_len > E.screen_cols) {
        msg_len = E.screen_cols;
    }
    if (msg_len && time(NULL) - E.status_msg_time < 5) {
        // only if the msg is less than 5 seconds old
        abuf_append(ab, E.status_msg, msg_len);
    }
}

void editor_refresh_screen() {
    editor_scroll();

    struct abuf ab = ABUF_INIT;
    // write an escape sequence to the terminal, which _always_ starts with:
    // `\x1b` - the first byte, the escape character
    // and followed by `[`
    // sequence: `Esc [ P(arameter)s J`

    // hide the cursor
    // https://vt100.net/docs/vt510-rm/DECTCEM.html
    abuf_append(&ab, "\x1b[?25l", 6);

    // erase all screen
    // instead, we clear line by line as we redraw them
    // abuf_append(&ab, "\x1b[2J", 4);

    // Reposition the cursor to top left
    // `ESC [ Pn ; Pn H`
    // default: `Esc [ 1;1 H`
    abuf_append(&ab, "\x1b[H", 3);

    // draw ~ on the first column of all rows
    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    // move cursor to the positions stored
    char buf[32];
    // +1 to E.cx and E.cy to convert from 0-indexed values to
    // the 1-indexed values used by terminal
    // notice the vertical `E.cy` - cy is no longer cursor position on screen
    // now cy is cursor position within the file
    // we need to re-position cursor on screen
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
             (E.rx - E.coloff) + 1);
    abuf_append(&ab, buf, strlen(buf));  // notice the `strlen`

    // show the cursor
    // https://vt100.net/docs/vt510-rm/DECTCEM.html
    abuf_append(&ab, "\x1b[?25h", 6);

    if (write(STDOUT_FILENO, ab.buffer, ab.len) == -1) die("write");

    abuf_free(&ab);
}

void editor_set_status_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);                                       // <stdarg.h>
    vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);  // handles va_arg()
    va_end(ap);
    // time() returns number of secs passed since midnight 01/01/1970
    E.status_msg_time = time(NULL);
}

/*** init ***/

/**
 * Initiate all fields in struct `E`
 */
void init_editor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.num_rows = 0;
    E.rows = NULL;
    E.filename = NULL;
    E.status_msg[0] = '\0';
    E.status_msg_time = 0;
    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
        die("get_window_size");
    }

    // make space for the status bar
    E.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: Ctrl-Q = quit");

    // read 1 byte from stdin into c until no more bytes to read
    // read() returns number of bytes read; returns 0 if reached EOF
    // use Ctrl-D to tell read() it has reached end of file
    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
