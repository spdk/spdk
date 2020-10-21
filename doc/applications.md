
# An Overview of SPDK Applications {#app_overview}

SPDK is primarily a development kit that delivers libraries and header files for
use in other applications. However, SPDK also contains a number of applications.
These applications are primarily used to test the libraries, but many are full
featured and high quality. The major applications in SPDK are:

- @ref iscsi
- @ref nvmf
- @ref vhost
- SPDK Target (a unified application combining the above three)

There are also a number of tools and examples in the `examples` directory.

The SPDK targets are all based on a common framework so they have much in
common. The framework defines a concept called a `subsystem` and all
functionality is implemented in various subsystems. Subsystems have a unified
initialization and teardown path.

# Configuring SPDK Applications {#app_config}

## Command Line Parameters {#app_cmd_line_args}

The SPDK application framework defines a set of base command line flags for all
applications that use it. Specific applications may implement additional flags.

Param    | Long Param             | Type     | Default                | Description
-------- | ---------------------- | -------- | ---------------------- | -----------
-c       | --config               | string   |                        | @ref cmd_arg_config_file
-d       | --limit-coredump       | flag     | false                  | @ref cmd_arg_limit_coredump
-e       | --tpoint-group-mask    | integer  | 0x0                    | @ref cmd_arg_limit_tpoint_group_mask
-g       | --single-file-segments | flag     |                        | @ref cmd_arg_single_file_segments
-h       | --help                 | flag     |                        | show all available parameters and exit
-i       | --shm-id               | integer  |                        | @ref cmd_arg_multi_process
-m       | --cpumask              | CPU mask | 0x1                    | application @ref cpu_mask
-n       | --mem-channels         | integer  | all channels           | number of memory channels used for DPDK
-p       | --master-core          | integer  | first core in CPU mask | master (primary) core for DPDK
-r       | --rpc-socket           | string   | /var/tmp/spdk.sock     | RPC listen address
-s       | --mem-size             | integer  | all hugepage memory    | @ref cmd_arg_memory_size
|        | --silence-noticelog    | flag     |                        | disable notice level logging to `stderr`
-u       | --no-pci               | flag     |                        | @ref cmd_arg_disable_pci_access.
|        | --wait-for-rpc         | flag     |                        | @ref cmd_arg_deferred_initialization
-B       | --pci-blacklist        | B:D:F    |                        | @ref cmd_arg_pci_blacklist_whitelist.
-W       | --pci-whitelist        | B:D:F    |                        | @ref cmd_arg_pci_blacklist_whitelist.
-R       | --huge-unlink          | flag     |                        | @ref cmd_arg_huge_unlink
|        | --huge-dir             | string   | the first discovered   | allocate hugepages from a specific mount
-L       | --logflag              | string   |                        | @ref cmd_arg_log_flags

### Configuration file {#cmd_arg_config_file}

SPDK applications are configured using a JSON RPC configuration file.
See @ref jsonrpc for details.

### Limit coredump {#cmd_arg_limit_coredump}

By default, an SPDK application will set resource limits for core file sizes
to RLIM_INFINITY.  Specifying `--limit-coredump` will not set the resource limits.

### Tracepoint group mask {#cmd_arg_limit_tpoint_group_mask}

SPDK has an experimental low overhead tracing framework.  Tracepoints in this
framework are organized into tracepoint groups.  By default, all tracepoint
groups are disabled.  `--tpoint-group-mask` can be used to enable a specific
subset of tracepoint groups in the application.

Note: Additional documentation on the tracepoint framework is in progress.

### Deferred initialization {#cmd_arg_deferred_initialization}

SPDK applications progress through a set of states beginning with `STARTUP` and
ending with `RUNTIME`.

If the `--wait-for-rpc` parameter is provided SPDK will pause just before starting
framework initialization. This state is called `STARTUP`. The JSON RPC server is
ready but only a small subset of commands are available to set up initialization
parameters. Those parameters can't be changed after the SPDK application enters
`RUNTIME` state. When the client finishes configuring the SPDK subsystems it
needs to issue the @ref rpc_framework_start_init RPC command to begin the
initialization process. After `rpc_framework_start_init` returns `true` SPDK
will enter the `RUNTIME` state and the list of available commands becomes much
larger.

To see which RPC methods are available in the current state, issue the
`rpc_get_methods` with the parameter `current` set to `true`.

For more details see @ref jsonrpc documentation.

### Create just one hugetlbfs file {#cmd_arg_single_file_segments}

Instead of creating one hugetlbfs file per page, this option makes SPDK create
one file per hugepages per socket. This is needed for @ref virtio to be used
with more than 8 hugepages. See @ref virtio_2mb.

### Multi process mode {#cmd_arg_multi_process}

When `--shm-id` is specified, the application is started in multi-process mode.
Applications using the same shm-id share their memory and
[NVMe devices](@ref nvme_multi_process). The first app to start with a given id
becomes a primary process, with the rest, called secondary processes, only
attaching to it. When the primary process exits, the secondary ones continue to
operate, but no new processes can be attached at this point. All processes within
the same shm-id group must use the same
[--single-file-segments setting](@ref cmd_arg_single_file_segments).

### Memory size {#cmd_arg_memory_size}

Total size of the hugepage memory to reserve. If DPDK env layer is used, it will
reserve memory from all available hugetlbfs mounts, starting with the one with
the highest page size. This option accepts a number of bytes with a possible
binary prefix, e.g. 1024, 1024M, 1G. The default unit is megabyte.

Starting with DPDK 18.05.1, it's possible to reserve hugepages at runtime, meaning
that SPDK application can be started with 0 pre-reserved memory. Unlike hugepages
pre-reserved at the application startup, the hugepages reserved at runtime will be
released to the system as soon as they're no longer used.

### Disable PCI access {#cmd_arg_disable_pci_access}

If SPDK is run with PCI access disabled it won't detect any PCI devices. This
includes primarily NVMe and IOAT devices. Also, the VFIO and UIO kernel modules
are not required in this mode.

### PCI address blacklist and whitelist {#cmd_arg_pci_blacklist_whitelist}

If blacklist is used, then all devices with the provided PCI address will be
ignored. If a whitelist is used, only whitelisted devices will be probed.
`-B` or `-W` can be used more than once, but cannot be mixed together. That is,
`-B` and `-W` cannot be used at the same time.

### Unlink hugepage files after initialization {#cmd_arg_huge_unlink}

By default, each DPDK-based application tries to remove any orphaned hugetlbfs
files during its initialization. This option removes hugetlbfs files of the current
process as soon as they're created, but is not compatible with `--shm-id`.

### Log flag {#cmd_arg_log_flags}

Enable a specific log type. This option can be used more than once. A list of
all available types is provided in the `--help` output, with `--logflag all`
enabling all of them. Additionally enables debug print level in debug builds of SPDK.

## CPU mask {#cpu_mask}

Whenever the `CPU mask` is mentioned it is a string in one of the following formats:

- Case insensitive hexadecimal string with or without "0x" prefix.
- Comma separated list of CPUs or list of CPU ranges. Use '-' to define range.

### Example

The following CPU masks are equal and correspond to CPUs 0, 1, 2, 8, 9, 10, 11 and 12:

~~~
0x1f07
0x1F07
1f07
[0,1,2,8-12]
[0, 1, 2, 8, 9, 10, 11, 12]
~~~
