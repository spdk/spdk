# Introduction

Starting with DPDK 22.11, PCI API are no longer public. In order to implement
out-of-tree driver like SPDK NVMe driver that is compatible with DPDK,
a copy of PCI API headers are required in SPDK.

`check_dpdk_pci_api.sh` script is intended simplify the maintenance of the
compatibility between SPDK copies of the headers and multiple DPDK versions.

## Arguments

The script has two optional positional arguments:
$1 - `check` or `fix` mode (default: check)
$2 - path to DPDK sources (default: DPDK submodule)

## Check mode

When calling the script, default is to check the diff of the in-tree headers with
DPDK and report any differences found. This is used for testing of current and
future DPDK versions.

## Fix mode

Similar to check mode, but additionally patches the differences to the
currently tested DPDK version. This should be done only for cosmetic changes,
not for changes that break compatibility.

## Workarounds for specific DPDK version

Any changes that should be applied to all copied headers have to be part of the
`check_dpdk_pci_api.sh`. For example referring to in-tree copied PCI headers
rather than system installed ones.

In rare cases there might be a need to apply a specific workaround for
particular DPDK PCI API version. Then a patch should be added in
`spdk_root/scripts/env_dpdk/<ver>` where ver is the matching DPDK version.

## Flow for adding support for new DPDK PCI API version

If API was changed, a new directory should be created `spdk_root/lib/env_dpdk/<ver>`
where ver is the appropriate DPDK version name. There the relevant headers should be copied.

Please note that the directories should match only the first DPDK version that modified the API.
Not every DPDK version requires new directory.
