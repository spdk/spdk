---
name: triage-ci-failure
description: >-
  Triage an SPDK CI failure -- download logs, identify the root
  cause, match to a known GitHub issue or draft a new one, and
  print a Gerrit comment
argument-hint: "GERRIT_PATCH_NUMBER"
triggers:

- user

---

# Triage SPDK CI Failure

Use this skill when a Gerrit patch has a CI failure that needs to be triaged.
Given a Gerrit change number (e.g. `28816`), the skill walks through:

1. Fetching the change details and finding the failed build
1. Downloading and analyzing the failed job logs
1. Searching GitHub issues for a matching known failure
1. Drafting a new issue if no match exists
1. Printing the Gerrit comment to post

## Prerequisites

- `gh` CLI authenticated to GitHub (`gh auth login`) with access
  to `spdk/spdk-ci` (private) and `spdk/spdk` (public)
- `curl` and `jq` available on the system
- Internet access to `review.spdk.io` and `github.com`

---

## Step 1: Fetch Gerrit Change Details

Query the Gerrit REST API (no auth required for public changes) and extract
the CI build messages.

```bash
CHANGE=<patch_number>

# Fetch change detail (response is prefixed with )]}' which must be stripped)
curl -s "https://review.spdk.io/changes/${CHANGE}/detail" \
  | tail -c +5 \
  | jq '.' > /tmp/gerrit_change_${CHANGE}.json
```

Parse the messages array to find the **latest** message from
`SPDK Automated Test System` that contains `Build failed`:

```bash
jq -r '
  .messages[]
  | select(.author.username == "spdk-bot")
  | select(.message | test("Build failed"))
  | {date: .date, message: .message, revision: ._revision_number}
' /tmp/gerrit_change_${CHANGE}.json
```

From the message, extract the **run ID** and **attempt number**. The format is:

```text
Build failed. Results: [RUN_ID/ATTEMPT](URL)
```

Extract with:

```bash
# Get the latest failed build message
FAILED_MSG=$(jq -r '
  [.messages[]
   | select(.author.username == "spdk-bot")
   | select(.message | test("Build failed"))]
  | last
  | .message' /tmp/gerrit_change_${CHANGE}.json)

RUN_ID=$(echo "$FAILED_MSG" | grep -oP 'actions/runs/\K[0-9]+')
ATTEMPT=$(echo "$FAILED_MSG" | grep -oP 'attempts/\K[0-9]+')
PATCHSET=$(jq -r '
  [.messages[]
   | select(.author.username == "spdk-bot")
   | select(.message | test("Build failed"))]
  | last
  | ._revision_number' /tmp/gerrit_change_${CHANGE}.json)

echo "Run ID: $RUN_ID, Attempt: $ATTEMPT, Patchset: $PATCHSET"
```

If there is **no** `Build failed` message, stop and tell the user -- there is
nothing to triage.

---

## Step 2: Identify Failed Jobs

List all jobs for the run and find which ones failed:

```bash
gh run view "$RUN_ID" --attempt "$ATTEMPT" -R spdk/spdk-ci
```

This shows a summary of all jobs with their status. Note the **job name(s)**
that show as `X` (failed). Common job names include:

| Job name pattern | What it tests |
|-----------------|---------------|
| `Common tests / autorun / *` | Core functional tests |
| `Common tests / pkgdep / *` | Package dependency tests |
| `Common tests / checkout_spdk` | Source checkout |
| `NVMe-oF RDMA tests / hpe-nvmf-rdma` | NVMe-oF RDMA on HPE hardware |
| `Job summary / merge_outputs` | Result aggregation (secondary failure) |
| `Job summary / report` | Result reporting (secondary failure) |

Note: `Job summary / merge_outputs` and `Job summary / report` failures are
usually **secondary** -- they fail because an upstream job failed. Focus on the
**primary** failed job (the test job that actually ran tests).

---

## Step 3: Download Failed Job Logs

Download the full logs zip via the GitHub API (the `gh run view --log-failed`
flag is unreliable and often returns empty output):

```bash
gh api "repos/spdk/spdk-ci/actions/runs/${RUN_ID}/attempts/${ATTEMPT}/logs" \
  > /tmp/ci_logs_${RUN_ID}.zip
```

List the contents to find the failed job's log file:

```bash
unzip -l /tmp/ci_logs_${RUN_ID}.zip
```

Log files are named like `26_NVMe-oF RDMA tests _ hpe-nvmf-rdma.txt`. Find
the one matching the failed job from Step 2.

**Important**: The **tail** of the log contains the actual failure. Early lines
contain expected test errors (e.g. memory map tests, lock contention tests)
that are normal. Always start from the end:

```bash
# Get the last 500 lines -- the real failure is at the end
unzip -p /tmp/ci_logs_${RUN_ID}.zip "*FAILED_JOB_PATTERN*" | tail -500

# Then narrow down to error lines in the tail
unzip -p /tmp/ci_logs_${RUN_ID}.zip "*FAILED_JOB_PATTERN*" | tail -500 \
  | grep -iE 'ERROR|FAILED|exit code|backtrace|assert|timeout|SIGTERM|SIGKILL|aborting'
```

Focus on:

1. **The backtrace section** near the end -- shows the exact failure path
1. **`*ERROR*` lines** in the last ~200 lines -- the real error, not test noise
1. **`Failing Code:` block** -- shows the exact line that triggered the trap
1. **`exit 1` / `exit code`** lines -- shows propagation path

---

## Step 4: Analyze the Failure

From the logs, identify:

1. **Failed test name** -- the specific test or sub-test that failed, e.g.
   `nvmf_rdma.nvmf_target_core_interrupt_mode.nvmf_ns_hotplug_stress`
1. **Error signature** -- the key error message, e.g.
   `nvme_qpair.c:722 nvme_qpair_abort_queued_reqs *ERROR*`
1. **Failure context** -- what was happening when the failure occurred
1. **Job/runner name** -- e.g. `hpe-nvmf-rdma`, `autorun-bdev-vm`

Build a set of **search keywords** from this analysis. Good keywords include:

- The failed test function/script name
- The error message (file:line, function name)
- The failed job name
- Key error strings (timeout, assertion, signal name)

---

## Step 5: Search GitHub Issues

Search for matching issues in `spdk/spdk`. Start with the `Intermittent Failure`
label, then broaden if nothing matches.

### 5a. Search by label + keywords

```bash
# Search with Intermittent Failure label and keywords
gh issue list -R spdk/spdk \
  -l "Intermittent Failure" \
  --search "<keyword from error>" \
  --state open \
  --limit 20
```

Try multiple keyword combinations:

```bash
# By job name
gh issue list -R spdk/spdk \
  -l "Intermittent Failure" \
  --search "hpe-nvmf-rdma" --state open
# By test name
gh issue list -R spdk/spdk \
  -l "Intermittent Failure" \
  --search "nvmf_ns_hotplug_stress" --state open
# By error function
gh issue list -R spdk/spdk \
  -l "Intermittent Failure" \
  --search "nvme_qpair_abort_queued_reqs" --state open
```

### 5b. Broaden search if no label match

```bash
# Search all open issues (not just Intermittent Failure)
gh issue list -R spdk/spdk --search "<keyword>" --state open --limit 20
# Also check closed issues (may have been fixed but regressed)
gh issue list -R spdk/spdk --search "<keyword>" --state closed --limit 10
```

### 5c. Read candidate issues

For each candidate issue, read its body to verify the failure matches:

```bash
gh issue view <issue_number> -R spdk/spdk --json title,body,labels
```

Compare:

- Is the failed **job name** the same?
- Is the **error message / function** the same or similar?
- Is the **test name** the same?
- Does the **failure pattern** match (same backtrace, same exit path)?

An issue matches if the error signature is substantially the same, even if the
patch under test is different (intermittent failures are by definition
patch-independent).

---

## Step 6: Report the Result

### 6a. If a matching issue was found

Print the Gerrit comment for the user to post on the patch:

```text
false positive: ISSUE_NUMBER
```

For example: `false positive: 3902`

Also explain briefly why this failure matches the known issue so the user can
verify the match.

### 6b. If no matching issue was found

Draft a new GitHub issue following the existing format. Present the draft to
the user for approval before creating.

**Title format**: `[test_name] Brief error description`

Examples of good titles (from existing issues):

- `[fio_dif_rand_params] heap-use-after-free in uring_sock_group_impl_poll`
- `[nvmf_target_disconnect_tc2] Unable to reset the controller`
- `[build failure] Unable to acquire the dpkg frontend lock`
- `NVMe-oF RDMA tests / hpe-nvmf-rdma` (job-level failure)

**Body format** (following existing convention from issues like #3902):

```markdown
# CI Intermittent Failure

<Job name> -> <test path that failed>.

<Brief description of the failure: what happened, where in the code, and the
immediate trigger.>

## Link to the failed CI build

[(<change>/<patchset>)<subject> - spdk/spdk-ci@<sha>](<github_actions_url>)

## Execution failed at

<dotted test path, e.g. nvmf_rdma.nvmf_target_core_interrupt_mode.nvmf_ns_hotplug_stress>

<relevant error log excerpt -- keep it focused, 20-50 lines max>
```

**Label**: `Intermittent Failure`

Show the draft to the user and ask for confirmation. Once approved, create:

```bash
gh issue create -R spdk/spdk \
  --title "<title>" \
  --body "<body>" \
  --label "Intermittent Failure"
```

After creation, print the Gerrit comment:

```text
false positive: NEW_ISSUE_NUMBER
```

---

## Quick Reference

```text
Triage Workflow:
  1. Fetch Gerrit change  -- curl review.spdk.io/changes/{N}/detail
  2. Extract failed build -- parse spdk-bot "Build failed" message
  3. Identify failed job  -- gh run view {RUN_ID} --attempt {ATTEMPT}
  4. Download logs        -- gh api .../logs > zip; unzip | tail
  5. Analyze failure      -- error signature, test name, context
  6. Search GitHub issues -- gh issue list -l "Intermittent Failure"
  7. Match or draft issue -- compare signatures; draft if new
  8. Print Gerrit comment -- "false positive: ISSUE_NUMBER"
```

## Common Failure Patterns

| Pattern | Cause | Keywords |
|---------|-------|----------|
| `qpair_abort_queued_reqs` | Qpair disconnect w/ queued I/O | `qpair abort` |
| `Timeout waiting for ...` | Slow hardware timeout | `timeout` |
| `heap-use-after-free` | ASAN use-after-free | `heap-use-after-free` |
| `dpkg frontend lock` | Package manager contention | `dpkg lock` |
| `ECONNRESET` | Artifact upload network reset | `ECONNRESET` |
| `not acquired by Runner` | Runner allocation failure | `Runner` |
| `HTTP 504` | Gateway timeout | `504` |
| `data digest errors` | NVMe-TCP data integrity | `data digest` |
| `Unable to reset the controller` | Controller reset failure | `reset` |

## Example Gerrit Comments

```text
# Simple reference to known issue
false positive: 3902

# With brief context (optional)
false positive: 3902
hpe-nvmf-rdma nvmf_ns_hotplug_stress -- qpair abort
```
