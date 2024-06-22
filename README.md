# Kilo, the minimal text editor

## Build & Run

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
- `ioctl` - control device
