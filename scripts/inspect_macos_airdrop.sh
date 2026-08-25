#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf 'This inspection helper requires macOS.\n' >&2
    exit 1
fi

for tool in rg strings nm codesign sw_vers; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Missing required tool: %s\n' "$tool" >&2
        exit 1
    fi
done

sharingd=/usr/libexec/sharingd
if [[ ! -x "$sharingd" ]]; then
    printf 'Native sharingd was not found at %s\n' "$sharingd" >&2
    exit 1
fi

printf 'macOS\n'
sw_vers
printf '\nArchitecture\n%s\n' "$(uname -m)"

printf '\nAirDrop-relevant imported symbols\n'
binary_arch="$(uname -m)"
if [[ "$binary_arch" == "arm64" ]]; then
    binary_arch=arm64e
fi
nm -arch "$binary_arch" -m "$sharingd" 2>/dev/null |
    rg 'SFAirDrop|kSFOperation(FileIcon|SmallFileIcon|TotalBytes)Key' || true

printf '\nAirDrop wire and implementation metadata\n'
strings -a "$sharingd" |
    rg -i 'SDAirDrop(Message\.Ask|BLEController|.*CompressionAdapter|.*NearField)|FileIcon|SmallFileIcon|EnableDVZip|receiverSupportsDVZip|application/x-dvzip|TotalBytes|SenderPseudonym|SenderPushToken|_airdrop\._tcp' |
    sort -u

printf '\nRelevant sharingd entitlements\n'
codesign -d --entitlements :- "$sharingd" 2>&1 |
    rg 'com\.apple\.(wifi\.awdl|bluetooth|sharing\.)' || true
