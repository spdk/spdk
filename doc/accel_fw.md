# Acceleration Framework {#accel_fw}

SPDK provides a framework for abstracting general acceleration capabilities
that can be implemented through plug-in modules and low-level libraries. These
plug-in modules include support for hardware acceleration engines such as
the Intel(R) I/O Acceleration Technology (IOAT) engine and the Intel(R) Data
Streaming Accelerator (DSA) engine. Additionally, a software plug-in module
exists to enable use of the framework in environments without hardware
acceleration capabilities. ISA/L is used for optimized CRC32C calculation within
the software module.

The framework includes an API for getting the current capabilities of the
selected module. See [`spdk_accel_get_capabilities`](https://spdk.io/doc/accel__engine_8h.html) for more details.
For the software module, all capabilities will be reported as supported. For the hardware modules, only functions
accelerated by hardware will be reported however any function can still be called, it will just be backed by
software if it is not reported as a supported capability.

## Acceleration Framework Functions {#accel_functions}

Functions implemented via the framework can be found in the DoxyGen documentation of the
framework public header file here [accel_engine.h](https://spdk.io/doc/accel__engine_8h.html)

## Acceleration Framework Design Considerations {#accel_dc}

The general interface is defined by `/include/accel_engine.h` and implemented
in `/lib/accel`.  These functions may be called by an SPDK application and in
most cases, except where otherwise documented, are asynchronous and follow the
standard SPDK model for callbacks with a callback argument.

If the acceleration framework is started without initializing a hardware module,
optimized software implementations of the functions will back the public API.
Additionally, if any hardware module does not support a specific function and that
hardware module is initialized, the specific function will fallback to a software
optimized implementation.  For example, IOAT does not support the dualcast function
in hardware but if the IOAT module has been initialized and the public dualcast API
is called, it will actually be done via software behind the scenes.

## Acceleration Low Level Libraries {#accel_libs}

Low level libraries provide only the most basic functions that are specific to
the hardware. Low level libraries are located in the '/lib' directory with the
exception of the software implementation which is implemented as part of the
framework itself. The software low level library does not expose a public API.
Applications may choose to interact directly with a low level library if there are
specific needs/considerations not met via accessing the library through the
framework/module. Note that when using the low level libraries directly, the
framework abstracted interface is bypassed as the application will call the public
functions exposed by the individual low level libraries. Thus, code written this
way needs to be certain that the underlying hardware exists everywhere that it runs.

The low level library for IOAT is located in `/lib/ioat`.  The low level library
for DSA is in `/lib/idxd` (IDXD stands for Intel(R) Data Acceleration Driver).
In `/lib/idxd` folder, SPDK supports to leverage both user space and kernel space driver
to drive DSA devices. And the following describes each usage scenario:

Leveraging user space idxd driver: The DSA devices are managed by the user space
driver in a dedicated SPDK process, then the device cannot be shared by another
process. The benefit of this usage is no kernel dependency.

Leveraging kernel space driver: The DSA devices are managed by kernel
space drivers. And the Work queues inside the DSA device can be shared among
different processes. Naturally, it can be used in cloud native scenario. The drawback of
this usage is the kernel dependency, i.e., idxd driver must be supported and loaded
in the kernel.

## Acceleration Plug-In Modules {#accel_modules}

Plug-in modules depend on low level libraries to interact with the hardware and
add additional functionality such as queueing during busy conditions or flow
control in some cases. The framework in turn depends on the modules to provide
the complete implementation of the acceleration component. A module must be
selected via startup RPC when the application is started. Otherwise, if no startup
RPC is provided, the framework is available and will use the software plug-in module.

### IOAT Module {#accel_ioat}

To use the IOAT engine, use the RPC [`ioat_scan_accel_engine`](https://spdk.io/doc/jsonrpc.html) before starting the application.

### IDXD Module {#accel_idxd}

To use the DSA engine, use the RPC [`idxd_scan_accel_engine`](https://spdk.io/doc/jsonrpc.html). With an optional parameter
of `-c` and providing a configuration number of either 0 or 1, users can determine which pre-defined configuration can be used.
With an optional parameter of `-k` to use kernel or user space driver. These pre-defined configurations determine how the DSA engine
will be setup in terms of work queues and engines.  The DSA engine is very flexible allowing for various configurations of
these elements to either account for different quality of service requirements or to isolate hardware paths where the back
end media is of varying latency (i.e. persistent memory vs DRAM).  The pre-defined configurations are as follows:

0: A single work queue backed with four DSA engines.  This is a generic configuration
that enables the hardware to best determine which engine to use as it pulls in new
operations.

1: Two separate work queues each backed with two DSA engines. This is another
generic configuration that is documented in the specification and allows the
application to partition submissions across two work queues. This would be useful
when different priorities might be desired per group.

There are several other configurations that are possible that include quality
of service parameters on the work queues that are not currently utilized by
the module. Specialized use of DSA may require different configurations that
can be added to the module as needed.

When a new channel starts, a DSA device will be assigned to the channel. The accel
idxd module has been tuned for the most likely best performance case. The result
is that there is a limited number of channels that can be supported based on the
number of DSA devices in the system.  Additionally, for best performance, the accel
idxd module will only use DSA devices on the same socket as the requesting
channel/thread.  If an error occurs on initialization indicating that there are no
more DSA devices available either try fewer threads or, if on a 2 socket system,
try spreading threads across cores if possible.

### How to use kernel idxd driver {#accel_idxd_kernel}

There are several dependencies to leverage kernel idxd driver for driving DSA devices.

1 Linux kernel support: To leverage kernel space idxd driver, you need to have a Linux kernel with
`idxd` driver loaded with scalable mode. And currently SPDK uses the character device while `idxd` driver is
enabled in the kernel. So when booting the machine, we need to add additional configuration in
the grub, i.e, revise the kernel boot commandline `intel_iommu=on,sm_on` with VT-d turned on in BIOS.

2 User library dependency: Users need to install `idxd-config` library. For example, users can
download the library from [idxd-config repo](https://github.com/intel/idxd-config). After the
library is installed, users can use the `accel-config` command to configure the work queues(WQs)
of the idxd devices managed by the kernel with the following steps:

```bash
accel-config disable-wq dsa0/wq0.1
accel-config disable-device dsa0
accel-config config-wq --group-id=0 --mode=dedicated --wq-size=16 --type=user --name="MyApp1"
 --priority=10 --block-on-fault=1 dsa0/wq0.1
accel-config config-engine dsa0/engine0.1 --group-id=0
accel-config enable-device dsa0
accel-config enable-wq dsa0/wq0.1
```

For more details on the usage of `idxd-config`, please refer to
[idxd-config usage](https://github.com/intel/idxd-config/tree/master/Documentation/accfg).

### Software Module {#accel_sw}

The software module is enabled by default. If no hardware engine is explicitly
enabled via startup RPC as discussed earlier, the software module will use ISA-L
if available for functions such as CRC32C. Otherwise, standard glibc calls are
used to back the framework API.

### Batching {#batching}

Batching is exposed by the acceleration framework and provides an interface to
batch sets of commands up and then submit them with a single command.  The public
API is consistent with the implementation however each plug-in module behaves
differently depending on its capabilities.

The DSA engine has complete support for batching all supported commands together
into one submission. This is advantageous as it reduces the overhead incurred in
the submission process to the hardware.

The software engine supports batching only to be consistent with the framework API.
In software there is no savings by batching sets of commands versus submitting them
individually.

The IOAT engine supports batching but it is only beneficial for `memmove` and `memfill`
as these are supported by the hardware.  All other commands can be batched and the
framework will manage all other commands via software.
