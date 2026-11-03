#!/bin/sh
# Run the upstream suite in the environment established by bin/package use.
set -eu
build=$1
shift
suite=$(mktemp -d "$build/ksh-work/tests.XXXXXXXX")
trap 'rm -rf "$suite"' EXIT
trap 'exit 1' HUP INT TERM

# Some regressions derive limits and variable names from ../sh and ../data.
# Preserve the source layout while patching only our private test directory.
for entry in "$PACKAGEROOT/src/cmd/ksh93"/*; do
    case ${entry##*/} in
        tests) cp -R "$entry" "$suite/tests" ;;
        *) ln -s "$entry" "$suite/" ;;
    esac
done
cd "$suite/tests"
while [ "$1" != -- ]; do
    # Strip a/src/cmd/ksh93/tests/ from the test-only patch paths.
    patch -p5 -F0 --batch --forward < "$1"
    shift
done
shift
export SHELL="$build/dtksh-test"
printf 'Regression-testing %s\n' "$SHELL"
"$SHELL" ./shtests --posix --utf8 "$@"
