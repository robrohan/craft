#!/bin/sh
# Bootstrap craft on a POSIX system.
#
#   sh build.sh                build ./craft with $CC (default: clang)
#   CC=tcc sh build.sh         build with tcc
#   sh build.sh --debug        ASan/UBSan build -> ./craft-debug
#   sh build.sh -o NAME        write the binary to NAME
#
# tcc is the compiler used for the Windows target; the source is kept within
# the intersection of what tcc and clang accept.
set -e

CC=${CC:-clang}
OUT=craft

# The code is C99; -std=gnu99 only widens POSIX header visibility (popen,
# putenv, st_mtim) on Linux.  tcc doesn't need or want the -std/-W flags.
case "$(basename "$CC")" in
    tcc|tcc.exe) : "${CFLAGS:=-Wall}" ;;
    *)           : "${CFLAGS:=-std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter}" ;;
esac

while [ $# -gt 0 ]; do
    case "$1" in
        --debug)
            CFLAGS="-std=gnu99 -g -O0 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined"
            OUT=craft-debug ;;
        -o) shift; OUT="$1" ;;
        *)  echo "build.sh: ignoring unknown argument '$1'" >&2 ;;
    esac
    shift
done

SRC="src/main.c src/util.c src/vars.c src/lex.c src/expand.c \
     src/parse.c src/rules.c src/build.c src/shell.c"

set -x
# shellcheck disable=SC2086
$CC $CFLAGS $SRC -o "$OUT"
