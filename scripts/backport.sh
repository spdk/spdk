#!/usr/bin/env bash

#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.

GERRIT_USER=""
HASHTAG=""
BACKPORT_DIR=""
SPDK_DIR=""
CHERRY_PICK_TEMP=""
FROM_BRANCH=""

function cleanup() {
	rm -rf "$BACKPORT_DIR"/cherry*
	rm -rf "$TMP_DIR"
}

function display_help() {
	echo " Usage: ${0##*/} [-u <gerrit_user>] [-t <hashtag>] [-d <spdk_dir>]

	Script automating Git operations to submit patches to different branches.

	  -d    Path for SPDK repository and temporary files used for backporting.
	  -t	Hashtag on patches to be backported and destination SPDK version (e.g. 23.05).
	  -u	Gerrit username.
	  -h	Show this help message.

	SHH authentication for provided Gerrit user is required.
	Merged commits from 'master' branch with provided hashtag (eg. 23.05) are cherry-picked
	to release branch (v23.05.x) in order they were merged. Skipping commits already present
	on the release branch.

	If merge conflict occurs during cherry-pick, it needs to be resolved manually.
	Then calling this script again will resume further cherry-picks.

	Example:
	./scripts/backport.sh -u gerrit_user -t 23.05 -d ./backports"
}

function reorder_commits() {
	local unordered_list=$1
	local from="v${HASHTAG}-rc1"

	git -C "$SPDK_DIR" fetch origin
	# Reorder commits to how they were merged to master
	git -C "$SPDK_DIR" show -s --format="%H" "$from^..HEAD" | tac > "$TMP_DIR/ordered_commits"

	mapfile -t u < "$unordered_list"
	mapfile -t o < "$TMP_DIR/ordered_commits"
	cp=()

	for ((i = 0; i < ${#u[@]}; i++)); do
		for ((j = 0; j < ${#o[@]}; j++)); do
			if [[ "${u[i]}" == "${o[j]}" ]]; then
				cp[j]=${u[i]}
			fi
		done
	done
	printf '%s\n' "${cp[@]}"
}

function fix_up_commit_msg() {
	local hash=$1

	git show -s --format="%B" "$hash" > $SPDK_DIR/msg
	# Add '(master)' info to link for original review
	sed -i '/Reviewed-on/ s/$/ (master)/' $SPDK_DIR/msg

	# Remove all tags added on original patches merged regarding reviewers and testers
	sed -i '/^Reviewed-by/d' "$SPDK_DIR/msg"
	sed -i '/^Community-CI/d' "$SPDK_DIR/msg"
	sed -i '/^Tested-by/d' "$SPDK_DIR/msg"
	echo "(cherry picked from commit $hash)" >> "$SPDK_DIR/msg"
	grep "^Change-Id" "$SPDK_DIR/msg" > "$SPDK_DIR/id"
	sed -i '/^Change-Id/d' "$SPDK_DIR/msg"
	cat "$SPDK_DIR/id" >> "$SPDK_DIR/msg"
	git commit -s --amend -m "$(cat $SPDK_DIR/msg)"
	rm -f "$SPDK_DIR/msg"
	rm -f "$SPDK_DIR/id"
}

while getopts "d:ht:u:" opt; do
	case "${opt}" in
		d)
			BACKPORT_DIR="$OPTARG"
			SPDK_DIR="$BACKPORT_DIR/spdk"
			CHERRY_PICK_TEMP="$BACKPORT_DIR/cherry-temp"
			;;
		h)
			display_help >&2
			exit 0
			;;
		t)
			HASHTAG="$OPTARG"
			FROM_BRANCH="v${HASHTAG}.x"
			;;
		u)
			GERRIT_USER="$OPTARG"
			;;
		*)
			display_help >&2
			exit 1
			;;
	esac
done

if [[ -z $HASHTAG || -z $GERRIT_USER || -z $BACKPORT_DIR ]]; then
	echo "Gerrit user, hashtag and directory are required."
	display_help
	exit 1
fi

# Gerrit interactive shells are disabled and attempts to connect with below
# command will result in error code 127, so parse the response instead.
if ! ssh -p 29418 $GERRIT_USER@review.spdk.io 2>&1 | grep -q "Welcome to Gerrit Code Review"; then
	echo "Could not connect to Gerrit"
	exit 1
fi

if [[ -d $SPDK_DIR && -e $SPDK_DIR/.git ]]; then
	echo "Updating SPDK repository."
	git -C "$SPDK_DIR" pull
else
	rm -rf "$SPDK_DIR"
	echo "Cloning SPDK repo"
	git clone "https://review.spdk.io/spdk/spdk" "$SPDK_DIR" --recurse-submodules
fi

cd "$SPDK_DIR"
# Enable git rerere to remember conflict resolution when creating commits to resubmit multiple times
git config --local rerere.enabled true
git config --local rerere.autoupdate true

if [[ ! -f $CHERRY_PICK_TEMP ]]; then
	echo "Creating commit list to cherry-pick"
	TMP_DIR="$BACKPORT_DIR/temp_files"

	if [[ ! -d $TMP_DIR ]]; then
		# If this is not the first run, then the direcotry might already exist
		mkdir "$TMP_DIR"
	fi

	# Get list of hash tagged patches on master branch
	ssh -p 29418 $GERRIT_USER@review.spdk.io gerrit query --format json \
		--current-patch-set project:spdk/spdk status:merged branch:"master" hashtag:"$HASHTAG" > "$TMP_DIR/master_${HASHTAG}_list"

	jq -r 'select(.id != null)|.id' "$TMP_DIR/master_${HASHTAG}_list" | sort > "$TMP_DIR/hash_id"

	# List of patches already submitted to the backporting branch
	ssh -p 29418 $GERRIT_USER@review.spdk.io gerrit query --format json \
		--current-patch-set project:spdk/spdk branch:"$FROM_BRANCH" > "$TMP_DIR/branch_list"
	jq -r 'select(.id != null)|.id' "$TMP_DIR/branch_list" | sort > "$TMP_DIR/branch_id"

	comm -13 "$TMP_DIR/branch_id" "$TMP_DIR/hash_id" > "$TMP_DIR/id_to_port"

	while read -r line; do
		jq -r ". | select(.id == "\"$line\"")" "$TMP_DIR/master_${HASHTAG}_list" \
			| jq -r '.currentPatchSet.revision'
	done < "$TMP_DIR/id_to_port" > "$TMP_DIR/hashtagged"

	reorder_commits "$TMP_DIR/hashtagged" > "$CHERRY_PICK_TEMP"

	git fetch origin
	git checkout $FROM_BRANCH

	git submodule update --init
fi

TOTAL=$(wc -l < "$CHERRY_PICK_TEMP")
echo "Total number of patches to backport: $TOTAL"

cat "$CHERRY_PICK_TEMP" > "$CHERRY_PICK_TEMP.cpy"

while read -r line; do
	cd "$SPDK_DIR"

	if [[ -n $(git status -suno) ]]; then
		if [[ -f "$BACKPORT_DIR/merge_conflict" ]]; then
			# Patch had conflict on previous script call, need to commit and fixup message
			id="$(cat $BACKPORT_DIR/merge_conflict)"
			git commit -C "$id"
			fix_up_commit_msg "$id"
			rm -f "$BACKPORT_DIR/merge_conflict"
		else
			echo "Changes pending, please resolve and commit them"
			exit 1
		fi
	fi

	# Removing new commit to cherry-pick from the list
	tail -n +2 "$CHERRY_PICK_TEMP" > "$CHERRY_PICK_TEMP.tmp" && mv "$CHERRY_PICK_TEMP.tmp" "$CHERRY_PICK_TEMP"

	REMAINING=$(wc -l < "$CHERRY_PICK_TEMP")
	NR=$((TOTAL - REMAINING))
	echo "Currently at $NR, remaining $REMAINING"

	if [ ! "$(git cherry-pick --rerere-autoupdate $line)" ]; then
		if [[ -z $(git status -suno) ]]; then
			echo "No changes to commit, most likely patch was already merged."
			git cherry-pick --abort
			continue
		fi

		# Git rerere might have already resolved the conflict
		if ! GIT_EDITOR=true git cherry-pick --continue; then
			echo "Merge conflict on patch:"
			echo "$line"
			echo "Use: git mergetool"
			echo "and then call this script again"
			echo "$line" > "$BACKPORT_DIR/merge_conflict"
			exit 1
		fi
	fi

	fix_up_commit_msg "$line"
	echo "Merged"
done < "$CHERRY_PICK_TEMP.cpy"
cd "$SPDK_DIR"
if [[ -f "$BACKPORT_DIR/merge_conflict" ]]; then
	# Patch had conflict on previous script call, need to commit and fixup message
	id="$(cat $BACKPORT_DIR/merge_conflict)"
	git commit -C "$id"
	fix_up_commit_msg "$id"
	rm -f "$BACKPORT_DIR/merge_conflict"
fi

cleanup

echo
echo "The script finished."
echo "Enter $SPDK_DIR, verify the commits at the top of $FROM_BRANCH"
echo "and then call 'git push origin HEAD:refs/for/$FROM_BRANCH' to submit them."
