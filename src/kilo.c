/*** includes ***/

#include <time.h>
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#define KILO_QUIT_TIMES 3
#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/**
 * 0x0001ffff
 * sets the upper 3 bits of the character to 0
 * similar to what ctrl does - strips bits 5 and 6 from input and sends to
 * terminal
 */
#define CTRL_KEY(k) ((k) & 0x1f)

enum EDITOR_KEY {
    BACKSPACE = 127,
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

enum EDITOR_HIGHLIGHT {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_RESERVED_KEYWORD,
    HL_RESERVED_TYPE,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH,
};

/*** data ***/

struct editor_syntax {
    char *filetype;
    // array of strings
    // each string contains a pattern to match a filename against
    char **filematch;
    char **reserveds;
    char *singleline_comment_start;
    int flags;
};

struct editor_row {
    int size;
    int rsize;    // size of render
    char *chars;  // dynamically allocated
    char *render;
    unsigned char *hl;  // highlighting, array of enum EDITOR_HIGHLIGHT
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
    int dirty;
    char *filename;
    char status_msg[80];
    time_t status_msg_time;
    struct editor_row *rows;
    struct editor_syntax *syntax;
    struct termios orig_termios;
};
struct editor_config E;

/*** filetypes ***/

char *C_HL_extensions[] = {".c", ".h", ".cpp", ".cc", NULL};
char *C_HL_reserveds[] = {
    "switch",    "if",      "while",   "for",    "break",
    "continue",  "return",  "else",    "struct", "union",
    "typedef",   "static",  "enum",    "class",  "case",

    "int|",      "long|",   "double|", "float|", "char|",
    "unsigned|", "singed|", "void|",   "time_t", NULL,
};
struct editor_syntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_reserveds,
        "//",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS,
    },
};

// length of the HLDB array
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editor_set_status_message(const char *fmt, ...);
void editor_refresh_screen();
char *editor_prompt(char *prompt, void (*callback)(char *, int));

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

/*** syntax highlighting ***/

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

/**
 * Go through all chars in an editor_row and
 * highlight them by setting each value in the hl array
 */
void editor_update_syntax(struct editor_row *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) {
        return;
    }

    char **reserveds = E.syntax->reserveds;

    char *scs = E.syntax->singleline_comment_start;
    int scs_len = scs ? strlen(scs) : 0;

    // beginning of a line is separator
    int prev_sep = 1;

    int in_string = 0;

    int i = 0;
    // while loops allows us to consume multiple chars each iteration
    while (i < row->rsize) {
        char c = row->render[i];
        // highlight type of previous character
        // previous highlight type either a number or separator
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    // when `\'` or `\"`
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) {
                    // if current character is the closing quote
                    in_string = 0;
                }
                i++;
                // when finished highlighting, the closing quote considered
                // a separator
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;  // store closing/opening quote
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;  // in the middle of highlighting something
                continue;
            }
        }

        if (prev_sep) {
            // keywords require a separator both _before_ and _after_
            int j;
            for (j = 0; reserveds[j]; j++) {
                int len = strlen(reserveds[j]);
                int reserved_type = reserveds[j][len - 1] == '|';
                if (reserved_type) len--;

                if (!strncmp(&row->render[i], reserveds[j], len) &&
                    is_separator(row->render[i + len])) {
                    memset(
                        &row->hl[i],
                        reserved_type ? HL_RESERVED_TYPE : HL_RESERVED_KEYWORD,
                        len);
                    i += len;
                    break;
                }
            }

            // check if the previous for loop is broken out
            if (reserveds[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }
}

int editor_syntax_to_color(int hl) {
    switch (hl) {
        case HL_COMMENT:
            return 36;  // cyan
        case HL_RESERVED_KEYWORD:
            return 33;  // yellow
        case HL_RESERVED_TYPE:
            return 32;  // green
        case HL_STRING:
            return 35;  // magenta
        case HL_NUMBER:
            // 31 is the red foreground color
            // https://en.wikipedia.org/wiki/ANSI_escape_code#3-bit_and_4-bit
            return 31;
        case HL_MATCH:
            return 34;
        default:
            // 39 is the white foreground color
            return 37;
    }
}

/**
 * Tries to match the current filename to one of the `filematch` fields
 * in HLDB
 */
void editor_select_syntax_highlight() {
    E.syntax = NULL;
    if (E.filename == NULL) {
        return;
    }

    char *ext = strchr(E.filename, '.');  // first occurrence
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editor_syntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;

                int row;
                for (row = 0; row < E.num_rows; row++) {
                    editor_update_syntax(&E.rows[row]);
                }
                return;
            }
            i++;
        }
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

/**
 * Convert a `render` index into a `chars` index
 */
int editor_row_rx_to_cx(struct editor_row *row, int rx) {
    int curr_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            curr_rx += (KILO_TAB_STOP - 1) - (curr_rx % KILO_TAB_STOP);
        }
        curr_rx++;
        if (curr_rx > rx) {
            return cx;
        }
    }

    return cx;
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

    editor_update_syntax(row);
}

void editor_insert_row(int at_row, char *s, size_t len) {
    if (at_row < 0 || at_row > E.num_rows) {
        return;
    }
    E.rows = realloc(E.rows, sizeof(struct editor_row) * (E.num_rows + 1));
    memmove(&E.rows[at_row + 1], &E.rows[at_row],
            sizeof(struct editor_row) * (E.num_rows - at_row));

    E.rows[at_row].size = len;
    E.rows[at_row].chars = malloc(len + 1);
    memcpy(E.rows[at_row].chars, s, len);
    E.rows[at_row].chars[len] = '\0';

    E.rows[at_row].rsize = 0;
    E.rows[at_row].render = NULL;
    E.rows[at_row].hl = NULL;

    editor_update_row(&E.rows[at_row]);

    E.num_rows++;
    E.dirty++;
}

void editor_free_row(struct editor_row *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editor_del_row(int at) {
    if (at < 0 || at >= E.num_rows) {
        return;
    }

    editor_free_row(&E.rows[at]);
    memmove(&E.rows[at], &E.rows[at + 1],
            sizeof(struct editor_row) * (E.num_rows - at - 1));
    E.num_rows--;
    E.dirty++;
}

void editor_row_insert_char(struct editor_row *row, int at, int c) {
    if (at < 0 || at > row->size) {
        at = row->size;  // by default append the char
    }
    // add 2 bytes - making room for the null byte
    // because the memmove below _shifts_ the existing sub line to the right
    // 1 byte;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    E.dirty++;
}

void editor_row_append_string(struct editor_row *row, char *s, size_t len) {
    // size does not include null byte
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    E.dirty++;
}

void editor_row_del_char(struct editor_row *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    E.dirty++;
}

/*** editor operations ***/

void editor_insert_char(int c) {
    if (E.cy == E.num_rows) {
        editor_insert_row(E.num_rows, "", 0);
    }
    editor_row_insert_char(&E.rows[E.cy], E.cx, c);
    E.cx++;
}

void editor_insert_new_line() {
    if (E.cx == 0) {
        // add a new empty line _BEFORE_ the current line
        editor_insert_row(E.cy, "", 0);
    } else {
        struct editor_row *row = &E.rows[E.cy];
        editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        // need to reassign the row pointer since `editor_inser_row()` calls
        // `realloc()` which may have invalidated the pointer
        row = &E.rows[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }

    E.cy++;
    E.cx = 0;
}

void editor_del_char() {
    if (E.cy == E.num_rows) {
        return;
    }
    if (E.cx == 0 && E.cy == 0) {
        return;
    }

    struct editor_row *row = &E.rows[E.cy];
    if (E.cx > 0) {
        editor_row_del_char(row, E.cx - 1);  // delete char before the cursor
        E.cx--;
    } else {
        E.cx = E.rows[E.cy - 1].size;
        editor_row_append_string(&E.rows[E.cy - 1], row->chars, row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
}

/*** file I/O ***/

/**
 * Convert the array of `struct editor_row`s into a single string
 */
char *editor_rows_to_string(int *buf_len) {
    int total_len = 0;
    int j;
    for (j = 0; j < E.num_rows; j++) {
        total_len += E.rows[j].size + 1;  // including the newline
    }
    *buf_len = total_len;

    char *buf = malloc(total_len);
    char *p = buf;
    for (j = 0; j < E.num_rows; j++) {
        memcpy(p, E.rows[j].chars, E.rows[j].size);
        p += E.rows[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editor_open(char *filename) {
    free(E.filename);
    // assuming you will free this memory yourself
    E.filename = strdup(filename);

    // detect filetype after filename changes
    editor_select_syntax_highlight();

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
        editor_insert_row(E.num_rows, line, line_len);
    }

    if (line) free(line);
    fclose(fp);
    E.dirty = 0;
}

void editor_save() {
    if (E.filename == NULL) {
        E.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editor_set_status_message("Save aborted!");
            return;
        }
        editor_select_syntax_highlight();
    }

    int len;
    char *buf = editor_rows_to_string(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        // <unistd.h>
        // cut off any additional data
        // add `0` bytes to reach the length if short
        if (ftruncate(fd, len) != -1) {
            // safer to write data to a file than passing `O_TRUNC`
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editor_set_status_message("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editor_set_status_message("Cannot save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editor_find_callback(char *query, int key) {
    // these are row numbers
    static int last_match = -1;
    static int direction = 1;
    static int saved_hl_line;      // which line's hl needs to be restored
    static char *saved_hl = NULL;  // hl to be restored

    if (saved_hl) {
        memcpy(E.rows[saved_hl_line].hl, saved_hl, E.rows[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        // if Enter or Esc
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) {
        direction = 1;  // search forward by default
    }
    int current = last_match;
    int r;
    for (r = 0; r < E.num_rows; r++) {
        current += direction;  // forward or backward
        if (current == -1) {
            current = E.num_rows - 1;  // wrap around?
        } else if (current == E.num_rows) {
            current = 0;
        }
        struct editor_row *row = &E.rows[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editor_row_rx_to_cx(row, match - row->render);
            // scroll to bottom of the screen, so that on next refresh
            // the matching line will be at top of the screen
            E.rowoff = E.num_rows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editor_find() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *prompt =
        "Search: %s (<Enter> search | <ESC> cancel | ← ↑ backward | → ↓ "
        "forward)";
    char *query = editor_prompt(prompt, editor_find_callback);

    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
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
 * Displays a prompt on the status bar, then lets user input a line of text
 * after the prompt
 * @param propmt: a format string containing %s
 */
char *editor_prompt(char *prompt, void (*callback)(char *, int)) {
    size_t buf_size = 128;
    char *buf = malloc(buf_size);

    size_t buf_len = 0;
    buf[0] = '\0';

    while (1) {
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buf_len != 0) {
                buf[--buf_len] = '\0';
            }
        } else if (c == '\x1b') {
            // <Esc> cancels the input prompt
            editor_set_status_message("");
            if (callback) {
                callback(buf, c);
            }
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buf_len != 0) {
                editor_set_status_message("");
                if (callback) {
                    callback(buf, c);
                }
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buf_len == buf_size - 1) {
                buf_size *= 2;
                buf = realloc(buf, buf_size);
            }
            buf[buf_len++] = c;
            buf[buf_len] = '\0';
        }
        if (callback) {
            callback(buf, c);
        }
    }
}

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
    static int quit_times = KILO_QUIT_TIMES;
    int c = editor_read_key();

    switch (c) {
        case '\r':
            editor_insert_new_line();
            break;
        case CTRL_KEY('q'):
            // enables <Ctrl-q> to quit
            if (E.dirty && quit_times > 0) {
                editor_set_status_message(
                    "WARNING!!! File has unsaved changes. Press Ctrl-Q %d more "
                    "times to quit.",
                    quit_times);

                quit_times--;
                return;
            }
            if (write(STDOUT_FILENO, "\x1b[2J", 4) == -1) {
                die("write");
            }
            if (write(STDOUT_FILENO, "\x1b[H", 3) == -1) {
                die("write");
            }
            exit(0);
            break;
        case CTRL_KEY('s'):
            editor_save();
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
        case CTRL_KEY('f'):
            editor_find();
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) {
                editor_move_cursor(ARROW_RIGHT);
            }
            editor_del_char();
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
        case CTRL_KEY('l'):
        case '\x1b':
            break;
        default:
            editor_insert_char(c);
            break;
    }

    // if user presses keys other than Ctrl-Q, resets quit_times
    quit_times = KILO_QUIT_TIMES;
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
            char *c = &E.rows[file_row].render[E.coloff];
            unsigned char *hl = &E.rows[file_row].hl[E.coloff];
            int curr_color = -1;  // default text color
            int j;
            for (j = 0; j < len; j++) {
                if (hl[j] == HL_NORMAL) {
                    if (curr_color != -1) {
                        // 39 is the default foreground color
                        abuf_append(ab, "\x1b[39m", 5);
                        curr_color = -1;
                    }
                    abuf_append(ab, &c[j], 1);
                } else {
                    int color = editor_syntax_to_color(hl[j]);
                    if (color != curr_color) {
                        curr_color = color;
                        char buf[16];
                        int clen =
                            snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abuf_append(ab, buf, clen);
                    }
                    abuf_append(ab, &c[j], 1);
                }
            }
            // reset text color to default
            abuf_append(ab, "\x1b[39m", 5);
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
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]", E.num_rows,
                       E.dirty ? "(modified)" : "");
    int rlen =
        snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                 E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.num_rows);
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
    E.dirty = 0;
    E.filename = NULL;
    E.status_msg[0] = '\0';
    E.status_msg_time = 0;
    E.syntax = NULL;

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

    editor_set_status_message(
        "HELP: Ctrl-F = find | Ctrl-S = save | Ctrl-Q = quit");

    // read 1 byte from stdin into c until no more bytes to read
    // read() returns number of bytes read; returns 0 if reached EOF
    // use Ctrl-D to tell read() it has reached end of file
    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
