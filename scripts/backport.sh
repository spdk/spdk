GERRIT_USER="$1"
BACKPORT_DIR="$2"
SPDK="$BACKPORT_DIR/spdk"
FILE="$BACKPORT_DIR/cherry-temp"
FROM="v19.04"
FROM_BRANCH="v19.04.x"
TO="master"
HASHTAG="19.04.1"


function test_spdk(){
        set -e
        cd $SPDK
        $SPDK/scripts/check_format.sh

        rm -rf ocf isa-l intel-ipsec-mb || yes
        git submodule update --init
        ./configure --enable-debug --enable-asan --enable-ubsan
        SKIP_DPDK_BUILD=$1 make -j 88
        $SPDK/test/unit/unittest.sh
        set +e
        echo "Tested"
        TESTED=1
}

function reorder_commits(){
        local unordered_list=$1

        # Reorder commits to how they were merged to master
        cd $SPDK
        git fetch origin
        git log origin/master $FROM..$TO | grep "^commit " | cut -c 8- | tac > $TMP/ordered_commits
        awk 'FNR==NR{a[$1];next}($1 in a){print}' $unordered_list $TMP/ordered_commits > $BACKPORT_DIR/cherry-pick
}

function fix_up_commit_msg(){
        local hash=$1

        git log --format=%B -n 1 $hash > $SPDK/msg
        # Add '(master)' info to link for original review
        sed -i '/Reviewed-on/ s/$/ (master)/' $SPDK/msg

        # Remove all tags added on original patches merged regarding reviewers and testers
        sed -i '/^Reviewed-by/d' $SPDK/msg
        sed -i '/^Tested-by/d' $SPDK/msg
        echo "(cherry picked from commit $hash)" >> $SPDK/msg
        git commit -s --amend -m "$(cat $SPDK/msg)"
        rm -f $SPDK/msg
}

if [ ! -d "$SPDK" ] ; then
        git clone "https://review.gerrithub.io/spdk/spdk" "$SPDK"
        cd $SPDK
        # Enable git rerere to remember conflict resolution when creating commits to resubmit multiple times
        git config --local rerere.enabled true
        git config --local rerere.autoupdate true
fi

if [[ ! -f $FILE ]]; then
        echo "Creating commit list to cherry-pick"
        TMP="$BACKPORT_DIR/temp_files"
        rm -rf $BACKPORT_DIR/cherry-pick
        rm -rf $TMP
        mkdir $TMP

        # Get list of hash tagged patches
        ssh -i ~/.ssh/id_rsa -p 29418 $GERRIT_USER@review.gerrithub.io gerrit query --format json --current-patch-set project:spdk/spdk status:merged hashtag:"$HASHTAG" > $TMP/hash_list
        cat $TMP/hash_list | jq -r ".id" | sort -u > $TMP/hash_id
        cp $TMP/hash_id $TMP/id_to_port
        # List of patches already submitted to the branch
        #ssh -i ~/.ssh/id_rsa -p 29418 tomzawadzki@review.gerrithub.io gerrit query --format json --current-patch-set project:spdk/spdk status:open branch:v19.04.x > $TMP/branch_list
        #cat $TMP/branch_list | jq -r ".id" | sort -u > $TMP/branch_id
        #awk 'FNR==NR {a[$0]++; next} !a[$0]' $TMP/branch_id $TMP/hash_id > $TMP/id_to_port

        rm -f $TMP/hashtagged
        while read line; do
                cat $TMP/hash_list | jq -r ". | select(.id == "\"$line\"")" | jq -r '.currentPatchSet.revision' >> $TMP/hashtagged
        done < $TMP/id_to_port

        reorder_commits $TMP/hashtagged
        rm -rf $TMP
        cp $BACKPORT_DIR/cherry-pick $FILE

        # First test from tip of FROM_BRANCH
        cd $SPDK
        git fetch origin
        git checkout $FROM_BRANCH
        git reset --hard
        test_spdk
fi

TOTAL=$(cat $BACKPORT_DIR/cherry-pick | wc -l)
echo "Total number of patches to backport: $TOTAL"

TESTED=0
while read line; do
        cd $SPDK

        if [[ ! -z $(git status -suno) ]]; then
                if [[ -f $BACKPORT_DIR/merge_conflict ]]; then
                        # Patch had conflict on previous script call, need to commit and fixup message
                        id="$(cat $BACKPORT_DIR/merge_conflict)"
                        git commit -C "$id"
                        fix_up_commit_msg "$id"
                        rm -f $BACKPORT_DIR/merge_conflict
                else
                        echo "Changes pending, please resolve and commit them"
                        exit 1
                fi
        fi

        if [ $TESTED -ne 1 ]; then
                test_spdk 1
        fi

        # Removing new commit to cherry-pick from the list
        tail -n +2 "$FILE" > "$FILE.tmp" && mv "$FILE.tmp" "$FILE"

        REMAINING=$(cat $FILE | wc -l)
        NR=$(($TOTAL - $REMAINING))
        echo "Currently at $NR, remaining $REMAINING"

        git cherry-pick --rerere-autoupdate "$line"
        if [ "$?" -ne 0 ]; then
                if [[ -z $(git status -suno) ]]; then
                        echo "No changes to commit, most likely patch was already merged."
                        git cherry-pick --abort
                        continue
                fi

                # Git rerere might have already resolved the conflict
                GIT_EDITOR=true git cherry-pick --continue
                if [ "$?" -ne 0 ]; then
                        echo "Merge conflict on patch:"
                        echo "$line"
                        echo "Use: git mergetool"
                        echo "and then call this script again"
                        echo "$line" > $BACKPORT_DIR/merge_conflict
                        exit 1
                fi
        fi

        fix_up_commit_msg "$line"
        echo "Merged"

        test_spdk 1
done < $FILE
cd $SPDK
if [[ -f $BACKPORT_DIR/merge_conflict ]]; then
        # Patch had conflict on previous script call, need to commit and fixup message
        id="$(cat $BACKPORT_DIR/merge_conflict)"
        git commit -C "$id"
        fix_up_commit_msg "$id"
        rm -f $BACKPORT_DIR/merge_conflict
fi
test_spdk 0