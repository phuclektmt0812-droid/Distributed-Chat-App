#!/bin/sh

executable=$1
log_file=$2

if [ -z "$executable" ] || [ -z "$log_file" ]; then
    echo "Usage: $0 EXECUTABLE LOG_FILE" >&2
    exit 2
fi

: > "$log_file" || {
    echo "Cannot write test log: $log_file" >&2
    exit 2
}

if [ ! -x "$executable" ]; then
    message="Test executable not found: $executable"
    printf '%s\n' "$message" | tee -a "$log_file"
    exit 2
fi

printf 'Running %s\n' "$executable" | tee -a "$log_file"

output_file=$(mktemp) || {
    echo "Could not create temporary output file." | tee -a "$log_file"
    exit 2
}

"$executable" > "$output_file" 2>&1
exit_code=$?
cat "$output_file" | tee -a "$log_file"
rm -f "$output_file"

exit "$exit_code"
