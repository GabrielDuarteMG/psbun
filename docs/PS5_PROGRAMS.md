# Running PS5 programs in a tab

PS5Term's own shell (the `1 console` tab) can run a PS5 ELF program, such as a homebrew tool or
a menu drawn with ncurses, right in the tab:

```text
/data$ run /data/homebrew/bin/mytool --flag "two words"
```

Its output shows as it runs and what you type goes to it. When it ends, PS5Term prints
`[mytool exited with status 0]` and the prompt returns.

## What you need

- **An ELF loader on port 9021**, as most PS5 homebrew setups already have (elfldr, or a
  loader that includes it). PS5Term cannot start a process itself. It sends a small launcher
  (`ps5term-run`) to the loader, and the launcher starts your program. If nothing answers on
  9021, `run` says so.
- **An ELF program.** Signed `.self` files from games do not run this way, the same as with
  any ELF loader.

## How it behaves

The PS5 has no pseudo-terminal (TTY) for programs, so PS5Term acts as the terminal:

| | |
|---|---|
| Normal output | Typing goes to PS5Term's line editor; **Enter** sends the line |
| Full screen (the program switches screens, as ncurses does) | Each key goes straight to the program, arrows and function keys included |
| **Ctrl+C** or the Interrupt button | Sends SIGINT |
| Closing the tab, or Reconnect | Stops the program |
| A launch that hangs | Ctrl+C gives the tab back; one tab launches at a time (a second waits its turn) |
| Screen size | `COLUMNS` and `LINES` give the tab's size when it starts; it does not follow a later resize |
| `TERM` | `xterm-256color`, with its terminfo entry in `/data/ps5term/terminfo` (`TERMINFO`) |

`run` works in PS5Term's own tabs, not in `ps5term@` SSH sessions.

## Bun (`bun`)

psbun is the native PS5 port of [Bun](https://bun.sh) in this fork. The full status is
[BUN_PORT.md](BUN_PORT.md). On a console, through the development launcher, Bun 1.4.4 has
printed `--version`, evaluated a script, run `Bun.$` and `Bun.spawn`, fetched HTTPS, completed
a TLSv1.3 handshake, and installed a package with `bun install` (including `--force` and
`bun add`). That was `/data/ps5term-dev/bun.elf`, not a PS5Term tab. `node_modules/.bin`
commands are not created (`/data` has no symlinks), and `bun install -g` still fails on those
links.

The shell's `bun` command runs like `run`, with the same needs (an ELF loader on port 9021)
and rules (own tabs only, no pipes or redirection):

```text
/data$ bun --version
/data$ bun -e "console.log(require('os').platform())"
```

`bun ARGS` runs `/data/ps5term/bun/bin/bun.elf ARGS`. Programs started through the loader
cannot see the app folder (`/app0`), so the first `bun` copies `/app0/assets/bun` to
`/data/ps5term/bun` (a few hundred MiB; the tab
waits while it copies). `/data/ps5term/bun/.installed` records the copied version; a PS5Term
update with a different Bun copies it again, and so does a copy that was interrupted. If the
copy fails, the command prints `bun: could not install Bun to /data/ps5term/bun: <reason>` and
exits with status 1. This copy has not been the console gate. A development `bun install` that
landed in `/data/ps5term/bun` was a cache, and it was deleted; later launcher runs use
`--cache-dir /data/ps5term-dev/cache`.

For every program it starts, the launcher sets `PS5TERM_TTY=1` (the PS5 Bun treats the tab's
socket as its terminal), `BUN_INSTALL=/data/ps5term/bun`, `PATH=/data/ps5term/bun/bin` and
`BUN_JSC_useJIT=0`; `HOME` stays `/data`. Anything that needs a real pseudo-terminal is
unavailable, because the PS5 kernel has no PTY driver (see below).

## For developers: ncurses and termios

Without a TTY, `tcgetattr` and `tcsetattr` fail (ENOTTY), so ncurses cannot switch to
`cbreak()` or `raw()` and holds every key until Enter. Add one file,
[`tooling/run-payload/ps5term_tty.c`](../tooling/run-payload/ps5term_tty.c), to your program and
wrap the two functions when linking:

```sh
prospero-clang -o app.elf app.c ps5term_tty.c -lncursesw \
    -Wl,--wrap=tcgetattr -Wl,--wrap=tcsetattr
```

On a real terminal nothing changes. On a socket (PS5Term, telnet shells) the terminal settings
live in memory, `cbreak()` succeeds and keys arrive one at a time. The file is permissively
licensed so any program can include it.

Tested on a PS5 (13.60) with ncurses 6.4: boxes, colours, arrow keys, Ctrl+C and exit status.

## Why not a real TTY?

The PS5 kernel has no pseudo-terminal driver: `posix_openpt` returns ENOSYS even to payloads.
PS5Term's Linux tabs have real TTYs because they run in PS5Term's own Linux kernel (see
[SUBSYSTEM.md](SUBSYSTEM.md)); programs built for Linux (riscv64) can run there instead.
