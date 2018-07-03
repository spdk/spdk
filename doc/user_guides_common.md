# Configuring SPDK Applications {#user_guides_common}

# Overview {#user_guides_common_overview}

This guide covers topics common to all applications that leverage SPDK's application framework.

## Command Line Parameters {#common_cmd_line_args}

The SPDK application framework defines a set of base command line flags for all applications that use it. Specific applications may implement additional flags.

Param    | Type     | Default                | Description
-------- | -------- | ---------------------- | -----------
-c       | string   |                        | @ref cmd_arg_config_file
-d       | flag     | false                  | disable coredump file creation
-e       | integer  | 0x0                    | tracepoint group hexadecimal mask for SPDK trace buffers
-g       | flag     | false                  | force creating just one hugetlbfs file
-h       | flag     | false                  | show all available parameters and exit
-i       | integer  | process PID            | shared memory ID
-m       | CPU mask | 0x1                    | application @ref cpu_mask
-n       | integer  | all channels           | number of memory channels used for DPDK
-p       | integer  | first core in CPU mask | master (primary) core for DPDK
-q       | flag     | false                  | disable notice level logging to `stderr`
-r       | string   | /var/tmp/spdk.sock     | RPC listen address
-s       | integer  | all hugepage memory    | memory size in MB for DPDK
-u       | flag     | false                  | @ref cmd_arg_disable_pci_access.
-w       | flag     | false                  | @ref cmd_arg_deferred_initialization
-B       | B:D:F    |                        | @ref cmd_arg_pci_blacklist_whitelist.
-W       | B:D:F    |                        | @ref cmd_arg_pci_blacklist_whitelist.
-L       | string   |                        | @ref cmd_arg_debug_log_flags
-f       | string   |                        | save pid to file under given path

### Configuration file {#cmd_arg_config_file}

Historically, the SPDK application framework was configured using a configuration file. This is still supported, but is
considered deprecated in favor of JSON RPC configuration. See @ref jsonrpc for details.

Note that `-c` and `-w` cannot be used at the same time.

### Deferred initialization {#cmd_arg_deferred_initialization}

SPDK applications progress through a set of states, including `STARTUP` and `RUNTIME`.

If `-w` parameter is provided SPDK will pause just before starting subsystem initialization. This state is called `STARTUP`.
The JSON RPC server is ready but list of commands is limited to only those that are needed to set application global parameters.
Those parameters can't be changed after SPDK application enters `RUNTIME` state. When client finishes preconfiguring SPDK subsystems
it needs to issue @ref rpc_start_subsystem_init RPC command to continue initialization process. After `rpc_start_subsystem_init`
returns `true` the SPDK will enter `RUNTIME` state and list of available commands will change again. From now on SPDK is ready
for further configuration.

To know what RPC methods are valid in current state issue `get_rpc_methods` with parameter `current` set to `true`.

For more details see @ref jsonrpc documentation.

### Disable PCI access {#cmd_arg_disable_pci_access}

If SPDK is run with PCI access disabled it won't detect any PCI devices, including NVMe, IOAT, NICs etc. Also VFIO and UIO
kernel modules are not needed anymore.

### PCI address blacklist and whitelist {#cmd_arg_pci_blacklist_whitelist}

Note that `-B` and `-W` cannot be used at the same time.

If blacklist is used all devices with provided PCI address will be ignored. If whitelist is used only those
devices will be probed. You can used `-B` or `-W` more than once to add more than one device to list.

### Debug log flags {#cmd_arg_debug_log_flags}

Use comma separated list of flags or use `-L all` to enable all debug flags. Run SPDK application with `-h` to get a list
of all valid flags. Debug flags are only available in debug builds of SPDK.

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
