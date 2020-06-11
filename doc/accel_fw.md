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
selected module. See [`spdk_accel_get_capabilities`](https://spdk.io/doc/accel__engine_8h.html) for more details. For the software module, all capabilities will be reported as supported. For the hardware modules, only functions accelerated by hardware will be reported however any function can still be called, it will just be backed by software if it is not reported as a supported capability.

# Acceleration Framework Functions {#accel_functions}

Functions implemented via the framework can be found in the DoxyGen documentation of the
framework public header file here [accel_engine.h](https://spdk.io/doc/accel__engine_8h.html)

# Acceleration Framework Design Considerations {#accel_dc}

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

# Acceleration Low Level Libraries {#accel_libs}

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
for DSA is in `/liv/idxd` (IDXD stands for Intel(R) Data Acceleration Driver).

# Acceleration Plug-In Modules {#accel_modules}

Plug-in modules depend on low level libraries to interact with the hardware and
add additional functionality such as queueing during busy conditions or flow
control in some cases. The framework in turn depends on the modules to provide
the complete implementation of the acceleration component. A module must be
selected via startup RPC when the application is started. Otherwise, if no startup
RPC is provided, the framework is available and will use the software plug-in module.

## IOAT Module {#accel_ioat}

To use the IOAT engine, use the RPC [`ioat_scan_accel_engine`](https://spdk.io/doc/jsonrpc.html) before starting the application.

## IDXD Module {#accel_idxd}

To use the DSA engine, use the RPC [`idxd_scan_accel_engine`](https://spdk.io/doc/jsonrpc.html) with an optional parameter of `-c` and provide a configuration number of either 0 or 1. These pre-defined configurations determine how the DSA engine will be setup in terms
of work queues and engines.  The DSA engine is very flexible allowing for various configurations of these elements to either account for different quality of service requirements or to isolate hardware paths where the back end media is of varying latency (i.e. persistent memory vs DRAM).  The pre-defined configurations are as follows:

0: Four separate work queues each backed with one DSA engine.  This is a generic
configuration that provides 4 portals to submit operations to each with a
single engine behind it providing some level of isolation as operations are
submitted round-robin.

1: Two separate work queues each backed with two DSA engines.  This is another
generic configuration that provides 2 portals to submit operations to and
lets the DSA hardware decide which engine to select based on loading.

There are several other configurations that are possible that include quality
of service parameters on the work queues that are not currently utilized by
the module. Specialized use of DSA may require different configurations that
can be added to the module as needed.

## Software Module {#accel_sw}

The software module is enabled by default. If no hardware engine is explicitly
enabled via startup RPC as discussed earlier, the software module will use ISA-L
if available for functions such as CRC32C. Otherwise, standard glibc calls are
used to back the framework API.

## Batching {#batching}

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
