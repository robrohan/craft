#!/bin/sh
# craft test runner (POSIX sh).
#
#   tests/run.sh              run every case with ./craft, diff against golden
#   tests/run.sh --compare    also run each case through GNU make and diff
#   tests/run.sh NNN-name ... run only the named cases
#
# A case is a directory tests/NNN-name/ containing:
#   Makefile        the makefile under test
#   cmd             (optional) arguments for one invocation; default: none
#   expected.out    golden combined stdout+stderr (work dir path -> '.')
#   expected.rc     (optional) expected exit code, default 0
#   setup.sh        (optional) sourced in the work dir before the run
#   *.in            (optional) copied into the work dir without the .in suffix
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CRAFT=${CRAFT:-$ROOT/craft}
TESTS=$ROOT/tests
COMPARE=0
FILTER=" "
# extra args prepended to every craft invocation (CI uses SHELL=sh on Windows)
EXTRAARGS=${EXTRAARGS:-}

for a in "$@"; do
    case "$a" in
        --compare) COMPARE=1 ;;
        *) FILTER="$FILTER$a " ;;
    esac
done

[ -x "$CRAFT" ] || { echo "no craft binary at $CRAFT (run build.sh)"; exit 1; }

pass=0
fail=0

prep_dir() {
    _d=$1 _w=$2
    cp "$_d/Makefile" "$_w"/ 2>/dev/null
    for f in "$_d"/*.in; do
        [ -e "$f" ] || continue
        b=$(basename "$f")
        cp "$f" "$_w/${b%.in}"
    done
    [ -f "$_d/setup.sh" ] && ( cd "$_w" && . "$_d/setup.sh" )
    return 0
}

run_case() {
    dir=$1
    name=$(basename "$dir")
    work=$(mktemp -d)
    prep_dir "$dir" "$work"

    args=""
    [ -f "$dir/cmd" ] && args=$(cat "$dir/cmd")
    want_rc=0
    [ -f "$dir/expected.rc" ] && want_rc=$(cat "$dir/expected.rc")

    cbase=$(basename "$CRAFT")
    ( cd "$work" && eval "\"$CRAFT\" $EXTRAARGS $args" ) >"$work/.got" 2>&1
    rc=$?
    sed -e "s#$work#.#g" -e "s#^$cbase:#craft:#g" -e "s# $cbase:# craft:#g" \
        "$work/.got" >"$work/.gotn"
    sed -e "s#$work#.#g" "$dir/expected.out" >"$work/.wantn" 2>/dev/null

    if cmp -s "$work/.gotn" "$work/.wantn" && [ "$rc" = "$want_rc" ]; then
        pass=$((pass + 1))
        echo "ok   $name"
    else
        fail=$((fail + 1))
        echo "FAIL $name  (rc=$rc want=$want_rc)"
        diff "$work/.wantn" "$work/.gotn" | sed 's/^/    /'
    fi

    if [ "$COMPARE" = 1 ] && command -v make >/dev/null 2>&1; then
        mwork=$(mktemp -d)
        prep_dir "$dir" "$mwork"
        ( cd "$mwork" && eval "make $args" ) >"$mwork/.mk" 2>&1
        sed -e "s#$mwork#.#g" "$mwork/.mk" >"$mwork/.mkn"
        if ! cmp -s "$mwork/.mkn" "$work/.gotn"; then
            echo "     note: differs from GNU make"
            diff "$mwork/.mkn" "$work/.gotn" | sed 's/^/       /'
        fi
        rm -rf "$mwork"
    fi

    rm -rf "$work"
}

for dir in "$TESTS"/[0-9]*; do
    [ -d "$dir" ] || continue
    if [ "$FILTER" != " " ]; then
        case "$FILTER" in
            *" $(basename "$dir") "*) ;;
            *) continue ;;
        esac
    fi
    run_case "$dir"
done

echo
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
