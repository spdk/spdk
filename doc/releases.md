# SPDK Releases {#releases}

All releases of SPDK are available through [GitHub releases](https://github.com/spdk/spdk/releases).
GitHub serves as the master repository for all changes, so the master branch always contains
the latest code.

## Cadence {#cadence}

There are three releases per year - at the end of January, May and September.
January is a Long Term Support (LTS) release, which is supported until the next LTS.
Other releases are supported until the following release.

## Version format {#version_format}

SPDK version names follow the format `YY.MM.vv`, where `YY` is the year, `MM` is the month,
and `vv` is a minor version number (omitted for 0). For example, the release in January 2026
is version 26.01. The minor version is reserved for patch releases made at a later date that
remain ABI compatible with the original release. Patch releases (e.g., 26.01.1) contain bug
fixes and security updates while maintaining ABI compatibility with the corresponding major release.

## Bug reports and backporting {#bug_reports}

Bug reports may be filed using [GitHub issues](https://github.com/spdk/spdk/issues) and should
indicate the version of SPDK in question (or specify a commit hash for the latest code). Bugs are always
fixed on the `master` branch first, but may be backported to supported release branches if:

- A user requests the backport by commenting on the issue stating to which version they'd like
  the bug backported
- A patch fixing the issue has a `YY.MM` hashtag on Gerrit
- The bug fix does not require a breaking ABI change

The backporting process is semi-automated using `scripts/backport.sh`, which cherry-picks commits
from master to release branches. For more details on backporting, see @ref backporting.

## Patch releases {#patch_releases}

Patch releases (e.g., 26.01.1) are made as needed to address critical bugs or security
issues. They maintain ABI compatibility with the corresponding major release.
Development for patch releases is done on the `vYY.MM.x` branch (e.g., `v26.01.x`).

## Changes throughout the release {#changes_during_release}

### Tracking {#tracking}

`CHANGELOG.md` and `deprecation.md` are used to notify users of modifications done
throughout the release. Ideally commits introducing a change should update those files,
as suggested by `scripts/check_format.sh`.
These files should include API, ABI, JSON RPC and major functional changes.

Only the section for the current release at the top of `CHANGELOG.md` should be updated,
sections for past releases should never be updated.

### Submodules {#submodules}

Some dependencies are tracked via git submodules and should be updated when upstream
creates new releases. For submodules that have a fork in the SPDK organization, it is
required to create new branches (e.g., spdk-25.11) from the upstream releases and propose
patches to the fork through Gerrit.

## Schedule {#schedule}

Releases are typically done on the last Friday of the month they are targeting. The schedule
can move up in case of any conflicts. Code freeze is done a week before the release.

### 1+ month before release {#month_before}

An announcement regarding specific dates for the upcoming release should be made on Slack.
From that point forward, a Gerrit hashtag 'YY.MM' (e.g., '26.01') should be used to prioritize
work on outstanding patches that need to be included in the release.

### 1 week before release {#week_before}

Code freeze begins a week before the release by creating a release branch (e.g., 'v26.01.x')
from the latest commit on the master branch. This branch should be tagged as a release candidate
(e.g., v26.01-rc1). The release branch should already include all major functional and ABI changes;
only bug fixes or documentation updates should be added after this point.

The first step after creating the branch must be updating [spdk-abi](https://github.com/spdk/spdk-abi)
repository with ABI matching the release candidate. This repository stores ABI definitions used for
compatibility checking between releases. The ABI definitions are generated using
`spdk-abi/generate_xml_abi.sh` and stored in a directory named after the release (e.g., `26.01.x`).

The commit following the release candidate should update `VERSION` and `CHANGELOG.md`
to reference the next release. That commit should be tagged with the `-pre` suffix (e.g., 'v26.05-pre').

After an LTS release, the master branch should update the major `SO_VER` by 1 for all libraries.
This allows for `SO_MINOR` updates throughout the support period of the LTS release.
This prevents scenarios where two versions exist with identical `SO_VER.SO_MINOR` but conflicting ABI.
For example, if the next SPDK release adds an ABI call that increases the minor version,
the LTS release may need only a subset of those additions. Increasing the major `SO_VER` after an LTS
allows future releases to update versions as needed, while allowing the LTS to increase
its minor version separately.

### Week of the release {#week_of_release}

Between code freeze and release, development against the master branch can continue without
disruptions. All patches submitted for review must be rebased on the latest master after the code freeze.
Merging to the master branch might be held off for patches that affect the ability to integrate
fixes for the release. The hashtag for the release should still be used to prioritize
any remaining commits. These commits should be first reviewed and merged on the master branch,
and only then backported to the release branch if necessary.

During this period, `CHANGELOG.md` entries should be reviewed, reordered alphabetically,
and formatted. Duplicated entries should be removed or merged, and formatting errors
should be fixed. This process ensures the changelog is clean and consistent for the release.

### Day of the release {#day_of_release}

The release should be tagged (e.g., 'v26.01') on the commit updating `VERSION` and `CHANGELOG.md`,
and followed by a commit updating them to the next patch version (e.g., 'v26.01.1-pre').
For an LTS release, the previous 'LTS' tag should be deleted from Gerrit and GitHub,
then recreated pointing to the latest release. Note that users and CI systems have to
fetch this tag with `--force` to see the result.

Creating a new release on GitHub should trigger a GitHub Action that automatically updates the
PyPI [SPDK package](https://pypi.org/project/spdk/).

### Until End of Life {#EOL}

For the duration of the supported period, the release should be maintained on its release
branch by backporting relevant fixes and keeping SPDK-CI running on that branch.
There is no set schedule for patch releases; they are done on a case-by-case basis.

After a release reaches End of Life (EOL), it is no longer maintained. No further
patches or security updates will be provided for EOL releases. Users are encouraged
to upgrade to a supported release version.
