#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause

if [[ $# -lt 2 ]]; then
	echo "Usage: $0 <old_tag> <new_tag>" >&2
	exit 1
fi

old_tag="${1}"
new_tag="${2}"

total_commit_count=$(git log --oneline "${old_tag}..${new_tag}" | wc -l)
total_contributor_count=$(git log --format='%ae' "${old_tag}..${new_tag}" | sort -u | wc -l)
lines_changed=$(git diff --shortstat "${old_tag}..${new_tag}" | awk '{print ($4+$6)}')

echo "Total commit count: $total_commit_count"
echo "Total contributor count: $total_contributor_count"
echo "Total lines changed (additions+deletions): $lines_changed"

old_emails=$(git log --format='%ae' "${old_tag}" | sort -u)
old_names=$(git log --format='%an' "${old_tag}" | sort -u)

# Find new contributors (neither email nor name in old_tag)
echo "New contributors:"
git log --format='%an|%ae' "${new_tag}" | sort -u | while IFS='|' read -r name email; do
	if ! echo "$old_emails" | grep -q "$email" \
		&& ! echo "$old_names" | grep -q "$name"; then
		echo "$name <$email>"
	fi
done | sort -u
