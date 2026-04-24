#!/bin/bash
# Test runner for Luna interpreter — dumps all outputs to a single file
# so you can visually verify correctness (nulls, errors, etc.)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

EXE="./luna"
if [ ! -f "$EXE" ]; then
    echo "Error: luna not found. Run 'make' first." >&2
    exit 1
fi

OUT_FILE="tests/tests_output.txt"
rm -f "$OUT_FILE"

PASSED=0
FAILED=0

echo "Running Luna regression tests..."

for test_file in tests/*.luna; do
    test_name="$(basename "$test_file")"
    printf "Running %s... " "$test_name"

    tmp_file="$(mktemp)"
    exit_code=0
    "$EXE" "$test_file" > "$tmp_file" 2>&1 || exit_code=$?

    # Strip DEBUG: lines from output (keep errors/vm messages)
    clean_output="$(grep -v '^DEBUG:' "$tmp_file" || true)"
    rm -f "$tmp_file"

    {
        echo "===== $test_name ====="
        echo "$clean_output"
        echo ""
    } >> "$OUT_FILE"

    if [ "$exit_code" -eq 0 ]; then
        echo "PASSED"
        ((PASSED++)) || true
    else
        echo "FAILED (exit code: $exit_code)"
        ((FAILED++)) || true
    fi
done

echo "================================"
if [ "$FAILED" -eq 0 ]; then
    echo "Results: $PASSED passed, $FAILED failed"
else
    echo "Results: $PASSED passed, $FAILED failed"
fi
echo "Full output written to: $OUT_FILE"
echo ""
cat "$OUT_FILE"

if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
