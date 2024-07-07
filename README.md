# Kilo, the minimal text editor

## Build & Run

- `nix develop` to enable Nix env with all dependencies
- `cmake . -B build`
- `cmake --build build`
- `./build/src/kilo` to run

## Dev Notes

- by default the terminal starts in **canonical mode**, a.k.a. **cooked mode**
  - keyboard input only sent to the program if user presses `<Enter>`
- we want the **raw mode**
- type `reset` then `<Enter>` in terminal resets it back to normal
- by default `ICANON` _is_ set
- **Escape sequence**
  - 3 or 4 bytes
  - starts with the `27` byte
    - which is the `<Esc>` key
- `<Ctrl-Z>` takes job to background
  - type `fg` to bring it to the front
- `<Ctrl-S>` - stop sending me output
  - `XOFF`
  - press `<Ctrl-Q>` to resume sending me output
    - `XON`
- `<Ctrl-C>` sends `SIGINT`
- `<Ctrl-Z>` sends `SIGSTP`
- `<Ctrl-C>` represents byte `3`
- `<Ctrl-D>` represents byte `3`
- `<Ctrl-Z>` represents byte `26`
- `IXON`
  - `I` - input flag
- `<Ctrl-S>` represents byte `19`
- `<Ctrl-Q>` represents byte `17`
- `<Ctrl-V>`
  - terminal waits for another character input
  - then sends that character literally
  - `IEXTEN` flag
- `<Ctrl-M>`
  - represented as `10` byte
  - `<Ctrl-J>` also represents `10`
  - so does `<Enter>`
- Terminal translates _any_ carriage returns (`13`, `\r`) inputted by user into newlines (`10`, `\n`)
  - `ICRNL`
    - `I` - input flag
    - `CR` - carriage return
    - `NL` - new line
- Terminal translates each newline (`"\n"`) users print into a carriage return _followed by_ a newline - `"\r\n"`
  - both `"\r"` and `"\n"` are required to start a new line
  - carriage return moves the cursor back to the beginning of the current line
  - newline moves cursor down a line, scrolling the screen if necessary
- Likely the only output processing feature turned on by default: `"\n"` -> `"\r\n"`
- `OPOST` - the output processing flag
- `BRKINT` - a **break condition** will cause a `SIGINT` sent to the program
  - similar to `<Ctrl-C>`
- `INPCK` - enables parity checking
- `ISTRIP` - sets the 8th bit of each input byte to be `0`
  - most likely already turned off
- `CS8` - a bit mask, _not_ a flag
  - sets the character size (CS) to 8 bits per byte
- `EAGAIN` - resource temporarily unavailable
- In C, bitmasks generally specified in hex
- use `J` command to [clear the screen](https://vt100.net/docs/vt100-ug/chapter3.html#ED)
- [VT100 User Guide](https://vt100.net/docs/vt100-ug/chapter3.html)
- If a tilde (`~`) is in a row - row is not part of the file and cannot contain any text
- To get window size of terminal,
  - `ioctl` - control device
  - the `TIOCGWINSZ` request
    - Terminal IOCtl Get WINdow SiZe
  - from `<sys/ioctl.h>`
- However, `ioctl()` is _not_ guaranteed to query the window size on all systems
- Use `"\x1b[K"` to clear the line after the `"~"`, instead of clearing the entire screen
- `snprintf()` - writes to the buffer string at most number of chars
- How to move cursors around?
  - keep track of cursor's x and y position
  - move cursor to position stored in `E.cx` and `E.cy` in `editor_refresh_screen()`
- Use `JKHL` to move cursors, like Vim
- Pressing `Ctrl` with another key
  - clears the 6th and 7th bits of the char presses with `Ctrl`
- `Page up` & `Page down` keys
  - `<esc>[5~` - `Page up`
  - `<esc>[6~` - `Page down`
- `Home` & `End` keys
  - `Home` could be sent as:
    - `<esc>[1~`
    - `<esc>[7~`
    - `<esc>[H`
    - `<esc>OH`
  - `End` could be sent as:
    - `<esc>[4~`
    - `<esc>[8~`
    - `<esc>[F`
    - `<esc>OF`
- `Delete` key - `<esc>[3~`
- Create a data type for storing a row of text in the editor
- `getline()` - read lines from a file while how much memory is allocated is unknown
  - takes care memory management for you
  - returns length of the read line; `-1` if error
- Strategy for vertical scrolling for long text
  - check if cursor has moved _out of_ the visible window
  - if so, adjust `E.rowoff` so that cursor is _just_ inside the visible window
- We allow users to scroll one line/column pass the edge - allowing users to insert lines/characters
- Rendering tabs
  - we want to render tabs as multiple spaces
  - **tab stop** - a column that's divisible by 8
  - each tab must advance the cursor forward at least one column
- `Page Up` & `Page Down` should scroll up/down an entire page
- Status bar
  - display file name & current row number
  - **Select Graphic Rendition (SGR)**
  - `ESC [ Ps ; . . . ; Ps m`
- status message & timestamp
