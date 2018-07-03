# General {#user_guides_common}

# Overview {#user_guides_common_overview}

This section covers topics common to whole SPDK.

## Parameters {#common_cmd_line_args}

SPDK library can take some parameters during startup. Some of them are common to all targets and are described here.
For target specific parameter refer to target documentation.

Param    | Type     | Default             | Description
-------- | -------- | ------------------- | -----------
-c       | string   |                     | configuration file.
-d       | flag     | false               | disable coredump file enabling
-e       | integer  | 0x0                 | tracepoint group hexadecimal mask for spdk trace buffers
-g       | flag     | false               | force creating just one hugetlbfs file
-h       | flag     | false               | show this usage and exit
-i       | integer  | process PID         | shared memory ID
-m       | CPU mask | 0x1                 | application @ref cpu_mask
-n       | integer  | all channels        | number of memory channels used for DPDK
-p       | integer  | first core          | master (primary) core for DPDK
-q       | flag     | false               | disable notice level logging to `stderr`
-r       | string   | /var/tmp/spdk.sock  | RPC listen address
-s       | integer  | all hugepage memory | memory size in MB for DPDK
-u       | flag     | false               | @ref cmd_arg_disable_pci_access.
-w       | flag     | false               | @ref cmd_arg_deferred_initialization
-B       | B:D:F    |                     | @ref cmd_arg_pci_blacklist_whitelist.
-W       | B:D:F    |                     | @ref cmd_arg_pci_blacklist_whitelist.
-L       | string   |                     | @ref cmd_arg_debug_log_flags
-f       | string   |                     | If given save pid to file under given path

### Configuration file {#cmd_arg_config_file}

Config files are legacy configuration method but you can still use them to provide initial configuration of SPDK.
This configuration method is going to be deprecated and eventualy removed. It is recomended to use @ref jsonrpc configuration
instead of config files.

### Disable PCI access {#cmd_arg_disable_pci_access}

If SPDK is run with PCI access disabled it won't detect any PCI device like NVMe, IOAT, NICs etc. Also any kernel
modules like VFIO or UIO is not needed anymore. This option is useful if multiple SPDK based applications are going
to be launched without using shared memory ID `-i` parameter.

### Deferred initialization {#cmd_arg_deferred_initialization}

If `-w` parameter is provided SPDK will pause just before starting subsystems initialization. This state is called `PRE INIT`.
The JSON RPC server is ready but list of commands is limited to only those that are needed to preconfigure subsystems. When
client finishes preconfiguring SPDK subsystems it needs to issue @ref rpc_start_subsystem_init RPC command to continue
initialization process. After `rpc_start_subsystem_init` returns `true` the SPDK will enter `RUNTIME` state and list of available
commands will change again. From now on SPDK is ready for further configuration.

To know what RPC methods are valid in current state issue `get_rpc_methods` with parameter `current` set to `true`.

For more details see @ref jsonrpc documentation.

### PCI address blacklist and white list {#cmd_arg_pci_blacklist_whitelist}

Note that `-B` and `-W` cannot be used at the same time.

If blacklist is used all devices with provided PCI address will be ignored. If whitelist is used only those
devices will be probed. You can used `-B` or `-W` more than once to add more than one device to list.

### Debug log flags {#cmd_arg_debug_log_flags}

Use comma separated list of flags or use `-L all` to enable all debug flags. Run SPDK application with `-h` to get
a list valid flags.

## CPU mask {#cpu_mask}

Whenever the `CPU mask` is metioned it is a string in one of the following formats:

- Case insensitive hexadecimal string with or without "0x" prefix.
- List of cpus or list of cpu ranges.


### Example

Following CPU masks are equal and correspond to CPUs 0, 1, 2, 8, 9, 10, 11 and 12:

~~~
0x1f07
0x1F07
1f07
[0,1,2,8-12]
[0, 1, 2, 8, 9, 10, 11, 12]
~~~
