BACKPORT_DIR="$2"
SPDK="$BACKPORT_DIR/spdk"
FILE="$BACKPORT_DIR/cherry-temp"
FROM="v19.04"
FROM_BRANCH="v19.04.x"
TO="master"
HASHTAG="19.04.1"

GERRIT_USER="$1"

function test_spdk(){
	set -e
	cd $SPDK
	$SPDK/scripts/check_format.sh

	git submodule update --init
	./configure --enable-debug --enable-asan --enable-ubsan
	SKIP_DPDK_BUILD=1 make -j 88
	$SPDK/test/unit/unittest.sh
	set +e
	echo "Tested"
}

function reorder_commits(){
	local unordered_list=$1

	cd $SPDK
	git fetch origin
	git reset --hard origin/$TO
	git clean -dffx
	git log $FROM..$TO | grep "^commit " > $TMP/commits
	tac $TMP/commits | cut -c 8- > $TMP/ordered_commits
	awk 'FNR==NR{a[$1];next}($1 in a){print}' $unordered_list $TMP/ordered_commits > $BACKPORT_DIR/cherry-pick
}

function locally_merged(){
	cd $SPDK
	git log $FROM..HEAD | grep "Change-Id" | cut -c 16- > $TMP/locally_merged
}

if [[ ! -f $FILE ]]; then
	echo "Creating commit list to cherry-pick"
	TMP="$BACKPORT_DIR/temp_files"
	rm -rf $TMP
	mkdir $TMP

	# Get list of hash tagged patches
	ssh -p 29418 $GERRIT_USER@review.gerrithub.io gerrit query --format json --current-patch-set project:spdk/spdk status:merged hashtag:"$HASHTAG" > $TMP/list

	# Remove already present on local branch
	cat $TMP/list | jq -r '.currentPatchSet.revision' > $TMP/hashtagged

	cd $BACKPORT_DIR
	git clone "https://review.gerrithub.io/spdk/spdk"

	reorder_commits $TMP/hashtagged

	rm -rf $TMP
	cp $BACKPORT_DIR/cherry-pick $FILE

	cd $SPDK
	git reset --hard origin/$FROM_BRANCH -q
	rm -rf ocf isa-l dpdk intel-ipsec-mb || yes
	git submodule update --init
	./configure
	make -j 88
	exit 1
fi

TOTAL=$(cat $BACKPORT_DIR/cherry-pick | wc -l)
echo "Total number of patches to backport: $TOTAL"

TESTED=0

while read line; do
	cd $SPDK

	if [[ ! -z $(git status -suno) ]]; then
		echo "Changes pending, please resolve and commit them"
		exit 1
	fi

	if [ $TESTED -ne 1 ]; then
		test_spdk
		TESTED=1
	fi

	tail -n +2 "$FILE" > "$FILE.tmp" && mv "$FILE.tmp" "$FILE"
	REMAINING=$(cat $FILE | wc -l)
	NR=$(($TOTAL - $REMAINING))
	echo "Currently at $NR, remaining $REMAINING"

	git cherry-pick -n "$line"
	cher=$?
	sed -i '/Reviewed-on/ s/$/ (master)/' $SPDK/.git/COMMIT_EDITMSG
	sed -i '/^Change-Id/,/^Reviewed-on/{//!d}' $SPDK/.git/COMMIT_EDITMSG
	sed -i '/Reviewed-on/ s/$/ (master)/' $SPDK/.git/MERGE_MSG
	sed -i '/^Change-Id/,/^Reviewed-on/{//!d}' $SPDK/.git/MERGE_MSG

	if [ "$cher" -ne 0 ]; then
		echo "Merge conflict on patch:"
		echo "$line"
		echo "Use: git mergetool && git commit -s"
		echo "and then call this scritp again"
		exit 1
	fi
	git commit -s --no-edit
	echo "Merged"
	test_spdk
	TESTED=1
done < $FILE
