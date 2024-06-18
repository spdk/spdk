# Backporting {#backporting}

In SPDK updating supported versions, other than the latest `master`, is carried out through
a process called "backporting", which is owned by core maintainers and backporters (currently
only Krzysztof Karaś). For such purposes, fixes to known issues or improvements to user experience,
without introducing new functionality, are usually chosen.

## How to backport to other SPDK maintained branches {#backporting_other_branches}

Backporting process consists of two phases:

### Selection

`selection` may be done by anyone who knows about a patch that should be introduced to an older
branch. A patch can be selected by adding a hashtag with desired destination release. This requires
rights to change `Hashtags` field on a Gerrit patch, so its owner, core maintainers or
somebody with Gerrit role `backporter` (currently only Krzysztof Karas `krzysztof.karas@intel.com`)
will be able to add necessary tag. Patch
[a4a0462](https://review.spdk.io/gerrit/c/spdk/spdk/+/17093) is a good example: `23.01` and `23.05`
were added in the `Hashtags` field of this patch, indicating that it should be pushed to the
corresponding branches `v23.01.x` and `v23.05.x`.

### Preparation

`preparation` may require some changes to the original code introduced by the patch, as it is taken
from a newer version of the repository and uploaded on top of an older one. Patch
[a4a0462](https://review.spdk.io/gerrit/c/spdk/spdk/+/17093) and its backport
[62b467b](https://review.spdk.io/gerrit/c/spdk/spdk/+/18981) are a good example,
because the code that they add is slightly different.
Additionally, during `preparation` commit message must be changed:

* footer should be stripped off of lines containing `Reviewed-by` and `Tested-by`,
* `Reviewed-on` line should contain, in parentheses, name of the branch it was pulled from,
* a line containing `(cherry picked from commit<original_commit_hash>)` should be added,
* if the code was modified heavily, then the body of the message should match that.

What should remain unchanged in commit message:

* title of the patch,
* `Change-Id`,
* Signed-off-by of the original patch author.

### Backporting script {#backporting_script}

In SPDK there is a script automating above process to some degree. It is located in
`scripts/backport.sh` and it requires users to have Gerrit username and SSH configuration,
as it pulls lists of existing commits to compare states of `master` and target branch, to which
backporting should be performed. The script will carry out most of commit message changes and try
to apply code as is. If it ever encounters a merge conflict, it will stop and save backporting
progress to a checkpoint file, from which user might resume after they are done fixing merge
conflicts.

## Updating submodules {#updating_submodules}

SPDK uses forks of other repositories as submodules. This is done for two reasons:

* to disable components that are unnecessary for SPDK to work,
* to introduce bug fixes and resolve compatibility issues.

The following example shows instructions on how DPDK would be updated from version 23.03 to 23.07,
but updating other submodules should be done by analogy.

### 1. Enter SPDK directory and update master branch

```bash
cd <path_to_spdk>; git checkout master; git pull
```

### 2. Enter DPDK submodule directory and update it

```bash
cd <path_to_spdk>/dpdk; git checkout master; git pull
```

### 3. Copy and checkout the latest fork

```bash
git branch -c spdk-23.07 spdk-23.07-copy; git checkout spdk-23.07-copy
```

Modifying a copy of the fork, instead of working directly on it, will make it easier to go back
in the future, if changes to the code are required later in the process. Otherwise, the branch
would need to be reset and local progress would be lost.

Notice that branches in submodules are named differently than SPDK branches. They usually consist
of `spdk-` prefix followed by the version of submodule it corresponds to. In this case, for DPDK
23.07 the branch is named `spdk-23.07`.

### 4. Cherry-pick patches from previous submodule fork and verify them

This requires browsing through [DPDK submodule repository](https://review.spdk.io/gerrit/q/project:spdk/dpdk)
and verifying whether all patches from previous fork are necessary and if further changes should
be introduced to ensure compatibility. In this case, patches from DPDK submodule branch
`spdk-23.03` should be pulled and applied to `spdk-23.07`. At this point three scenarios
are possible:

* All patches are necessary and sufficient to make new version of the submodule compatible with
  SPDK.
* Some patches are unnecessary, because DPDK code has been changed and it matches SPDK needs,
  so they can be skipped.
* Patches from previous submodule fork are insufficient and further changes to the DPDK code are
  required. That means either modifications to existing, pulled patches or creating new changes.
  The latter might require debugging or manually checking dependencies.

Patches for submodules should be pushed to appropriate branch:

```bash
git push https://review.spdk.io/gerrit/spdk/dpdk HEAD:refs/for/spdk-23.07
```

Patches uploaded to Gerrit need to go through usual testing and review process before they are
merged. Only after that the submodule update may be continued. Then, after the fork is ready,
the submodule update may be carried out:

```bash
cd <path_to_spdk>; git checkout master; git pull
cd <path_to_spdk>/dpdk; git checkout spdk-23.07; git pull
cd ..; git add dpdk; git commit --signoff
git push https://review.spdk.io/gerrit/spdk/spdk HEAD:refs/for/master
```

Above commands update SPDK master branch, check out the newest fork of the DPDK submodule, then
add it to a commit and finally push the submodule update to Gerrit.
