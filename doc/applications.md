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

Historically, the SPDK applications were configured using a configuration file.
This is still supported, but is considered deprecated in favor of JSON RPC
configuration. See @ref jsonrpc for details.

Note that `-c` and `-w` cannot be used at the same time.

### Deferred initialization {#cmd_arg_deferred_initialization}

SPDK applications progress through a set of states beginning with `STARTUP` and
ending with `RUNTIME`.

If the `-w` parameter is provided SPDK will pause just before starting subsystem
initialization. This state is called `STARTUP`. The JSON RPC server is ready but
only a small subsystem of commands are available to set up initialization
parameters. Those parameters can't be changed after the SPDK application enters
`RUNTIME` state. When the client finishes configuring the SPDK subsystems it
needs to issue the @ref rpc_start_subsystem_init RPC command to begin the
initialization process. After `rpc_start_subsystem_init` returns `true` SPDK
will enter the `RUNTIME` state and the list of available commands becomes much
larger.

To see which RPC methods are available in the current state, issue the
`get_rpc_methods` with the parameter `current` set to `true`.

For more details see @ref jsonrpc documentation.

### Disable PCI access {#cmd_arg_disable_pci_access}

If SPDK is run with PCI access disabled it won't detect any PCI devices. This
includes primarily NVMe and IOAT devices. Also, the VFIO and UIO kernel modules
are not required in this mode.

### PCI address blacklist and whitelist {#cmd_arg_pci_blacklist_whitelist}

Note that `-B` and `-W` cannot be used at the same time.

If a blacklist is used all devices with the provided PCI address will be
ignored. If a whitelist is used only those devices will be probed. You can used
`-B` or `-W` more than once to add more than one device to list.

### Debug log flags {#cmd_arg_debug_log_flags}

Use a comma separated list of flags or use `-L all` to enable all debug flags.
Run SPDK application with `-h` to get a list of all valid flags. Debug flags are
only available in debug builds of SPDK.

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
