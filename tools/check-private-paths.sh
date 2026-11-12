#!/usr/bin/env bash
# Fails if a file listed in tools/private-paths.txt is tracked or was ever
# committed in the checked history.
#   tools/check-private-paths.sh              files tracked in HEAD
#   tools/check-private-paths.sh REV_RANGE    files touched by those commits,
#                                             e.g. origin/main..HEAD or --all
# PRIVATE_PATHS_FILE overrides the list (the pre-push hook reads it from the
# commit that is pushed).
set -euo pipefail

list="${PRIVATE_PATHS_FILE:-$(dirname "${BASH_SOURCE[0]}")/private-paths.txt}"
specs=()
while IFS= read -r line; do
  [[ -z "$line" || "$line" == \#* ]] && continue
  specs+=(":(glob,icase)**/$line" ":(glob,icase)$line")
done < "$list"
[[ ${#specs[@]} -gt 0 ]] || { echo "no patterns in $list" >&2; exit 2; }

if [[ $# -eq 0 ]]; then
  found="$(git ls-files -- "${specs[@]}")"
else
  found="$(git log "$@" --name-only --format= -- "${specs[@]}" | sort -u)"
fi

if [[ -n "$found" ]]; then
  echo "Private-only files must not be in the public repository:" >&2
  echo "$found" | sed 's/^/  /' >&2
  exit 1
fi
