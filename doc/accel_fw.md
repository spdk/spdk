# Acceleration Framework {#accel_fw}

SPDK provides a framework for abstracting general acceleration capabilities
that can be implemented through plug-in modules and low-level libraries. These
plug-in modules include support for hardware acceleration engines such as
the Intel(R) I/O Acceleration Technology (IOAT) engine and the Intel(R) Data
Streaming Accelerator (DSA) engine. Additionally, a software plug-in module
exists to enable use of the framework in environments without hardware
acceleration capabilities. ISA/L is used for optimized CRC32C calculation within
the software module.

## Acceleration Framework Functions {#accel_functions}

Functions implemented via the framework can be found in the DoxyGen documentation of the
framework public header file here [accel.h](https://spdk.io/doc/accel_8h.html)

## Acceleration Framework Design Considerations {#accel_dc}

The general interface is defined by `/include/spdk/accel.h` and implemented
in `/lib/accel`.  These functions may be called by an SPDK application and in
most cases, except where otherwise documented, are asynchronous and follow the
standard SPDK model for callbacks with a callback argument.

If the acceleration framework is started without initializing a hardware module,
optimized software implementations of the operations will back the public API. All
operations supported by the framework have a backing software implementation in
the event that no hardware accelerators have been enabled for that operation.

When multiple hardware modules are enabled the framework will assign each operation to
a module based on the order in which it was initialized. So, for example if two modules are
enabled, IOAT and software, the software module will be used for every operation except those
supported by IOAT.

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
for DSA and IAA is in `/lib/idxd` (IDXD stands for Intel(R) Data Acceleration Driver and
supports both DSA and IAA hardware accelerators). In `/lib/idxd` folder, SPDK supports the ability
to use either user space and kernel space drivers. The following describes each usage scenario:

Leveraging user space idxd driver: The DSA devices are managed by the SPDK user space
driver in a dedicated SPDK process, then the device cannot be shared by another
process. The benefit of this usage is no kernel dependency.

Leveraging kernel space driver: The DSA devices are managed by kernel
space drivers. And the Work queues inside the DSA device can be shared among
different processes. Naturally, it can be used in cloud native scenario. The drawback of
this usage is the kernel dependency, i.e., idxd kernel driver must be supported and loaded
in the kernel.

## Acceleration Plug-In Modules {#accel_modules}

Plug-in modules depend on low level libraries to interact with the hardware and
add additional functionality such as queueing during busy conditions or flow
control in some cases. The framework in turn depends on the modules to provide
the complete implementation of the acceleration component. A module must be
selected via startup RPC when the application is started. Otherwise, if no startup
RPC is provided, the framework is available and will use the software plug-in module.

### IOAT Module {#accel_ioat}

To use the IOAT module, use the RPC [`ioat_scan_accel_module`](https://spdk.io/doc/jsonrpc.html) before starting the application.

### DSA Module {#accel_dsa}

The DSA module supports the DSA hardware and relies on the low level IDXD library.

To use the DSA module, use the RPC
[`dsa_scan_accel_module`](https://spdk.io/doc/jsonrpc.html). By default, this
will attempt to load the SPDK user-space idxd driver. To use the built-in
kernel driver on Linux, add the `-k` parameter. See the next section for
details on using the kernel driver.

The DSA hardware supports a limited queue depth and channels. This means that
only a limited number of `spdk_thread`s will be able to acquire a channel.
Design software to deal with the inability to get a channel.

#### How to use kernel idxd driver {#accel_idxd_kernel}

There are several dependencies to leverage the Linux idxd driver for driving DSA devices.

1 Linux kernel support: You need to have a Linux kernel with the `idxd` driver
loaded. Further, add the following command line options to the kernel boot
commands:

```bash
intel_iommu=on,sm_on
```

2 User library dependency: Users need to install the developer version of the
`accel-config` library. This is often packaged, but the source is available on
[GitHub](https://github.com/intel/idxd-config). After the library is installed,
users can use the `accel-config` command to configure the work queues(WQs) of
the idxd devices managed by the kernel with the following steps:

Note: this library must be installed before you run `configure`

```bash
accel-config disable-wq dsa0/wq0.1
accel-config disable-device dsa0
accel-config config-wq --group-id=0 --mode=dedicated --wq-size=128 --type=user --name="MyApp1"
 --priority=10 --block-on-fault=1 dsa0/wq0.1
accel-config config-engine dsa0/engine0.0 --group-id=0
accel-config config-engine dsa0/engine0.1 --group-id=0
accel-config config-engine dsa0/engine0.2 --group-id=0
accel-config config-engine dsa0/engine0.3 --group-id=0
accel-config enable-device dsa0
accel-config enable-wq dsa0/wq0.1
```

DSA can be configured in many ways, but the above configuration is needed for use with SPDK.
Before you can run using the kernel driver you need to make sure that the hardware is bound
to the kernel driver and not VFIO.  By default when you run `setup.sh` DSA devices will be
bound to VFIO.  To exclude DSA devices, pass a whitespace separated list of DSA devices BDF
using the PCI_BLOCKED parameter as shown below.

```bash
sudo PCI_BLOCKED="0000:04:00.0 0000:05:00.0" ./setup.sh
```

Note: you might need to run `sudo ./setup.sh reset` to unbind all drivers before performing
the step above.

### Software Module {#accel_sw}

The software module is enabled by default. If no hardware module is explicitly
enabled via startup RPC as discussed earlier, the software module will use ISA-L
if available for functions such as CRC32C. Otherwise, standard glibc calls are
used to back the framework API.

### dpdk_cryptodev {#accel_dpdk_cryptodev}

The dpdk_cryptodev module uses DPDK CryptoDev API to implement crypto operations.
The following ciphers and PMDs are supported:

- AESN-NI Multi Buffer Crypto Poll Mode Driver: RTE_CRYPTO_CIPHER_AES128_CBC
- Intel(R) QuickAssist (QAT) Crypto Poll Mode Driver: RTE_CRYPTO_CIPHER_AES128_CBC,
  RTE_CRYPTO_CIPHER_AES128_XTS
  (Note: QAT is functional however is marked as experimental until the hardware has
  been fully integrated with the SPDK CI system.)
- MLX5 Crypto Poll Mode Driver: RTE_CRYPTO_CIPHER_AES256_XTS, RTE_CRYPTO_CIPHER_AES512_XTS

To enable this module, use [`dpdk_cryptodev_scan_accel_module`](https://spdk.io/doc/jsonrpc.html),
this RPC is available in STARTUP state and the SPDK application needs to be run with `--wait-for-rpc`
CLI parameter. To select a specific PMD, use [`dpdk_cryptodev_set_driver`](https://spdk.io/doc/jsonrpc.html)

### Module to Operation Code Assignment {#accel_assignments}

When multiple modules are initialized, the accel framework will assign op codes to
modules by first assigning all op codes to the Software Module and then overriding
op code assignments to Hardware Modules in the order in which they were initialized.
The RPC `accel_get_opc_assignments` can be used at any time to see the current
assignment map including the names of valid operations.  The RPC `accel_assign_opc`
can be used after initializing the desired Hardware Modules but before starting the
framework in the event that a specific override is desired.  Note that to start an
application and send startup RPC's use the `--wait-for-rpc` parameter and then use the
`framework_start_init` RPC to continue. For example, assume the DSA Module is initialized
but for some reason the desire is to have the Software Module handle copies instead.
The following RPCs would accomplish the copy override:

```bash
./scripts/rpc.py dsa_scan_accel_module
./scripts/rpc.py accel_assign_opc -o copy -m software
./scripts/rpc.py framework_start_init
./scripts/rpc.py accel_get_opc_assignments
{
  "copy": "software",
  "fill": "dsa",
  "dualcast": "dsa",
  "compare": "dsa",
  "crc32c": "dsa",
  "copy_crc32c": "dsa",
  "compress": "software",
  "decompress": "software"
}
```

To determine the name of available modules and their supported operations use the
RPC `accel_get_module_info`.
