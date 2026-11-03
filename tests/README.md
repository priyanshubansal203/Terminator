# Demo commands and screenshots

One page per task in the specification. Each page lists commands to type into Terminator, what
they show, and screenshots of a real run. Task 11 (running bash scripts) is optional and not
covered.

| Task | Page | Shows |
|---|---|---|
| 1 | [X11 GUI and tabs](task01-gui-and-tabs.md) | tab bar, independent tabs, activity marker |
| 2 | [Built-in and external commands](task02-commands.md) | `cd`, `fork`/`execvp`, input to a running program, `command not found`, `$?`, `source`, `history` |
| 3 | [Multi-line input and quoting](task03-multiline-quoting.md) | `\` continuation, multi-line quotes, nested `$(...)` and backticks |
| 4 | [I/O redirection](task04-redirection.md) | `<`, `>`, both, missing input file, concurrent readers |
| 5 | [Pipes](task05-pipes.md) | long pipelines, pipes with redirection, `xargs rm` |
| 6 | [multiWatch](task06-multiwatch.md) | parallel commands, required output format, Ctrl+C |
| 7 | [Hotkeys](task07-hotkeys.md) | `~/.terminatorrc`, invalid and conflicting bindings, `bind` |
| 8 | [Signals and job control](task08-jobs.md) | Ctrl+Z, `jobs`, `bg`, `fg`, Ctrl+C, `cmd &` |
| 9 | [Searchable history](task09-history.md) | Ctrl+R search rules, history shared between tabs (process mode) |
| 10 | [File-name completion](task10-completion.md) | single match, numbered list, spaces, directories |

## Setup

```sh
make
./tests/setup.sh            # creates ~/terminator-demo with the sample files (or: ./tests/setup.sh DIR)
./terminator                # or: ./terminator useProcess=True
```

Then, inside Terminator:

```
cd ~/terminator-demo
```

`setup.sh` creates the following:

| File | Used by |
|---|---|
| `myprog.c` | a program that numbers the lines it reads (Tasks 2, 4, 5) |
| `somefile.txt` | unsorted fruit names (Tasks 4, 5) |
| `infile.txt` | three lines of input (Tasks 4, 5) |
| `large.txt` | the numbers 1–200,000 (Task 4) |
| `a.tmp`, `b.tmp`, `c.tmp` | files for `xargs rm` to delete (Task 5) |
| `abc.txt`, `abcd.txt`, `def.txt`, `notes for class.txt`, `projects/src/main.c` | completion targets (Task 10) |
| `greet.sh` | a script for `source` (Task 2) |

Run `setup.sh` again to restore the files a demo deleted or changed.

## How the screenshots were made

Every screenshot is an unedited capture of `./terminator` running on a real X display. A small
Xlib helper sent the keystrokes listed on each page to the Terminator window with `XSendEvent`
and captured the window with `XGetImage`. Each run started Terminator in `~/terminator-demo`,
with a fresh `HOME` holding the sample files. The tab labels therefore show `~/terminator-demo`,
and nothing from a personal history file appears.
