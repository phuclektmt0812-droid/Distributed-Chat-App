#!/bin/sh

test_dir=$1
tests=$2
log_file=$3

if [ -z "$test_dir" ] || [ -z "$tests" ] || [ -z "$log_file" ]; then
    echo "Usage: $0 TEST_DIRECTORY TEST_NAMES LOG_FILE" >&2
    exit 2
fi

: > "$log_file" || {
    echo "Cannot write test log: $log_file" >&2
    exit 2
}

log_line() {
    printf '%s\n' "$1" | tee -a "$log_file"
}

log_line "Test run started: $(date '+%Y-%m-%d %H:%M:%S')"

for name in $tests; do
    executable="$test_dir/$name"

    log_line ""
    log_line "========== $name =========="

    if [ ! -x "$executable" ]; then
        log_line "Test executable not found: $executable"
        exit 1
    fi

    output_file=$(mktemp) || {
        log_line "Could not create temporary output file."
        exit 2
    }

    "$executable" > "$output_file" 2>&1
    exit_code=$?
    cat "$output_file" | tee -a "$log_file"
    rm -f "$output_file"

    if [ "$exit_code" -ne 0 ]; then
        log_line "Test failed: $name (exit code $exit_code)"
        exit "$exit_code"
    fi
done

log_line ""
log_line "All $(printf '%s\n' $tests | wc -l | tr -d ' ') test programs passed."
