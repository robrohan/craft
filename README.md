# craft

A small `make` clone in portable C99 designed to work on both Windows and Unix.
It builds with [tcc](https://bellard.org/tcc/) on Windows (without using
Cygwin/MSYS/WSL), and with `cc`/`clang` on macOS and Linux. The goal is to be
as close to GNU make as is practical while accepting that recipe lines run
under `cmd.exe` (or PowerShell) on Windows instead of `/bin/sh`.

## Quick Start

- Bootstrap the build using `build.bat` or `build.sh` from the building sections
- Once that is done you can simply run

```bash
craft [options] [target ...] [VAR=VALUE ...]
```

## Building

Because you need to compile craft before you can use craft, there are two bootstrap
build files `build.bat` and `build.sh` for Windows and *nix respectfully.

| Platform      | Command                                                                     |
|---------------|-----------------------------------------------------------------------------|
| Windows (tcc) | `zero.bat` -> `craft.exe`                                                   |
| macOS / Linux | `sh build.sh` -> `craft` (set `CC=` to override; `--debug` adds ASan/UBSan) |
| Self-host     | `craft -f craft.mk` (also `make -f craft.mk`)                               |

All code is writte in plain C99 located in `src/` and it doesn't have any 3rd party
dependencies.  Because I am using tcc on windows, the source could should stays compatible
with tcc and clang at the minimum.

## Running recipes on Windows

Since Makefile is really just a nice way to run shell commands, the commands will be
different between Window and Mac / Linux. Each recipe line in the Craftfile is handed
to one shell process, attempting to match GNU make's default.

The shell is chosen from the `SHELL` variable:

- unset = `cmd.exe` on Windows, `/bin/sh` on POSIX
- ends in `sh` / `bash` = `<shell> -c "<line>"`
- ends in `cmd` = `<shell> /c "<line>"`
- `powershell` / `pwsh` = `<shell> -NoProfile -Command "<line>"`

So a portable makefile uses recipes that works in both shells is maybe kind of possible
with similar items (`echo`, redirects, `&&`), but will almost assuradly need to have one
for Unix and one for Windows.

`.ONESHELL` attempts to run the whole recipe in a single shell invocation.

## What Works (mostly)

**Variables** — `=`, `:=`, `::=`, `?=`, `+=`, `!=`; recursive vs simple flavor;
`override`, `export`, `unexport`, `undefine`; `define ... endef`. Precedence:
built-in < makefile < environment < command line < `override` (`-e` lifts the
environment above the makefile). Target-specific variables
(`debug: CFLAGS += -g`).

**Basic automatic variables** — `$@ $< $^ $? $* $+` and the `$(@D) $(@F)` ... `D`/`F`
variants.

**Rules** — explicit rules, multiple targets, prerequisite accumulation across
lines, `;` inline recipes, `@` / `-` / `+` recipe prefixes, order-only
prerequisites (`|`), pattern rules (`%.o: %.c`), static pattern rules
(`$(OBJ): %.o: %.c`), implicit-rule chaining, double-colon rules, a small set of
built-in rules and variables (disable with `-r`).

**Special targets** — `.PHONY`, `.PRECIOUS`, `.SECONDARY`, `.SILENT`,
`.IGNORE`, `.DELETE_ON_ERROR`, `.ONESHELL`, `.EXPORT_ALL_VARIABLES`.

**Functions** — subst patsubst strip findstring filter filter-out sort word
wordlist words firstword lastword dir notdir suffix basename addsuffix addprefix
join wildcard realpath abspath foreach if or and call value origin flavor error
warning info shell.

**Conditionals & include** — `ifeq` / `ifneq` / `ifdef` / `ifndef` / `else` /
`else ifeq` / `endif`; `include`, `-include`, `sinclude` with `-I` search.

**Command line** — `-f -C -I -n -k -s -B -e -i -r -p -q -j -h -v`, long option
aliases, `--`, and `VAR=value` overrides. `$(MAKE)` recursion propagates
`MAKELEVEL` and the short flags via `MAKEFLAGS`.

## Deliberate omissions

- `-j` parallel builds — accepted and ignored; builds are serial.
- `vpath` / `VPATH`, suffix rules (`.SUFFIXES`), archive members.
- `$(eval)`, `.SECONDEXPANSION`, `define`d call-macros with recipe semantics
  beyond simple text, `--warn-undefined-variables`, `guile`, `load`.
- Remaking included makefiles is single-pass, not iterative.

## Deviations from GNU make

- Timestamps use the highest resolution the OS offers (nanoseconds on  macOS/Linux,
100 ns on Windows); a prerequisite is newer only if strictly greater.
* Recipe quoting for a non-default `SHELL` is best-effort (single-quote wrap for
`sh`, `/c " ... "` for cmd); exotic quoting may differ.
* Diagnostics are prefixed `craft:` and error lines read
`craft: *** [file:line: target] Error N` (attempt to match modern GNU make 4.x,
not macOS shipped version 3.8)

## Tests

```
sh tests/run.sh              # golden-file tests
sh tests/run.sh --compare    # also diff each case against GNU make
```
Each `tests/NNN-name/` holds a `Makefile`, optional `cmd` / `setup.sh` / `*.in`,
and a golden `expected.out`.
