# Security Policy

## Reporting a Vulnerability

If you believe you have identified a potential security issue, please
[submit a vulnerability report on GitHub](https://github.com/spdk/spdk/security/advisories/new).
The report will be private, only accessible by the submitter and the SPDK CVE
sub-team. Do not discuss it on Slack or open a public GitHub issue.

## CVE Process

The SPDK CVE process is designed to be simple yet effective. The process is here
to help make sure that security vulnerabilities are dealt with efficiently, and
with the least amount of advertising possible. An SPDK CVE sub-team exists to
handle this process. Issues can come from anywhere, but when one comes to the
attention of the community it should be reported privately as described above.
This is to ensure that we don't advertise possible vulnerabilities
before they are fully understood.

Sub-team members are responsible for the following process:

1. Upon receiving a vulnerability report, the sub-team will discuss the potential
   issue.
2. The sub-team will identify the resources required to investigate and determine
   the scope of the vulnerability, including a fix or workaround.
3. When the fix or workaround has been identified, the sub-team will approve it
   or ask for further investigation.
4. Once approved, the patch will be submitted via normal channels without verbose
   information on the vulnerability itself. It will simply state what the patch is
   doing (not why).
5. Once the patch is merged, the most recent official release will be used as a
   baseline for a maintenance release containing only security fixes. Fixes for
   multiple vulnerabilities may be combined into one maintenance release.
6. SPDK uses GitHub as its CVE Numbering Authority (CNA). The sub-team will
   request a CVE ID through the GitHub security advisory. Once the release is
   tagged, the sub-team will publish the advisory, after which GitHub publishes
   the CVE details.
7. After the advisory has been published, an announcement will be made on the
   mailing list with more information about the patch and the fix so that community members
   can decide for themselves what their exposure is and when, if at all, they should
   move to the new release.

## Current CVEs and Threat Models

The list of SPDK CVE entries and the threat models by use case are on the
[SPDK CVE and threat modeling page](https://spdk.io/cve_threat/).
