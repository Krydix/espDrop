#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'Usage: %s [duration-seconds] [output-directory]\n' "$0"
}

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf 'This capture helper requires macOS.\n' >&2
    exit 1
fi

duration="${1:-60}"
if [[ ! "$duration" =~ ^[1-9][0-9]*$ ]]; then
    usage >&2
    exit 2
fi

timestamp="$(date -u '+%Y%m%dT%H%M%SZ')"
output_dir="${2:-build/macos-airdrop-oracle/$timestamp}"
mkdir -p "$output_dir"

children=()
cleanup() {
    local pid
    for pid in "${children[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${children[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

{
    printf 'captured_at_utc=%s\n' "$timestamp"
    printf 'duration_seconds=%s\n' "$duration"
    printf 'host_arch=%s\n' "$(uname -m)"
    sw_vers
    printf '\nsharingd_sha256='
    shasum -a 256 /usr/libexec/sharingd | awk '{print $1}'
    printf '\nawdl0\n'
    ifconfig awdl0 2>&1 || true
} >"$output_dir/environment.txt"

/usr/bin/log stream \
    --style ndjson \
    --level debug \
    --predicate 'process == "sharingd"' \
    >"$output_dir/unified-log.ndjson" \
    2>"$output_dir/unified-log.stderr" &
children+=("$!")

/usr/bin/dns-sd -includeAWDL -B _airdrop._tcp local. \
    >"$output_dir/bonjour-airdrop.txt" \
    2>"$output_dir/bonjour-airdrop.stderr" &
children+=("$!")

# A packet capture records timing, endpoints, TLS records, retransmissions, and
# packet sizes. AirDrop application bodies remain encrypted. Opening the BPF
# device normally requires root; failure is retained instead of making this
# otherwise read-only helper prompt for a password.
/usr/sbin/tcpdump -i awdl0 -U -s 0 -w "$output_dir/awdl0.pcap" \
    >"$output_dir/tcpdump.stdout" \
    2>"$output_dir/tcpdump.stderr" &
children+=("$!")

printf 'Capturing native AirDrop activity for %s seconds.\n' "$duration"
printf 'Output: %s\n' "$output_dir"
printf 'Initiate and complete exactly one native transfer now.\n'
sleep "$duration"
cleanup
trap - EXIT INT TERM

if [[ ! -s "$output_dir/awdl0.pcap" ]]; then
    rm -f "$output_dir/awdl0.pcap"
    printf 'Packet capture unavailable; see tcpdump.stderr. Unified logs and Bonjour data were retained.\n'
fi

printf 'Capture complete: %s\n' "$output_dir"
