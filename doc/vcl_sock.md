# VCL Socket Backend

This document describes how to build and use the SPDK `vcl` socket backend on Linux.

## Overview

The `vcl` backend adds a new SPDK socket implementation based on the VPP Communications Library (VCL). It is available alongside the existing `posix` and `uring` socket backends.

Typical use cases:

- SPDK TCP transports running over a VPP dataplane
- NVMe/TCP targets and initiators using VCL sockets
- Environments that want user space networking through VPP instead of the Linux TCP stack

## Build Requirements

- Linux
- A VPP source tree with `libvppcom` built
- SPDK configured with `--with-vcl=<path-to-vpp-tree>`

The SPDK build expects:

- VCL headers under `<VPP_TREE>/src`
- `libvppcom` under `<VPP_TREE>/build-root/build-vpp-native/vpp/lib/x86_64-linux-gnu`

## Build SPDK With VCL

Example:

```bash
./configure --with-vcl=/path/to/vpp --with-uring
make -j
```

This enables the `sock_vcl` module and links SPDK against `libvppcom`.

## Runtime Requirements

At runtime, the VPP shared libraries must be reachable by the SPDK process. The SPDK build adds an rpath for the default VPP build output location, so no extra `LD_LIBRARY_PATH` is required when using that standard tree layout.

The VCL backend also requires a valid VCL configuration file for each SPDK process that uses it. In practice this is usually provided through:

```bash
export VCL_CONFIG=/path/to/vcl.conf
```

The exact VCL configuration depends on the VPP deployment model.

## Select The VCL Socket Backend

After starting an SPDK application with RPC enabled, select the socket implementation:

```bash
scripts/rpc.py sock_set_default_impl -i vcl
```

Then continue with the normal transport setup. Example for an NVMe/TCP target:

```bash
scripts/rpc.py framework_start_init
scripts/rpc.py framework_wait_init
scripts/rpc.py nvmf_create_transport -t TCP
```

The same socket backend selection also applies to SPDK clients that create TCP connections.

## Example NVMe/TCP Flow

Target side:

```bash
scripts/rpc.py sock_set_default_impl -i vcl
scripts/rpc.py framework_start_init
scripts/rpc.py framework_wait_init
scripts/rpc.py bdev_null_create Null0 67108864 4096
scripts/rpc.py nvmf_create_transport -t TCP
scripts/rpc.py nvmf_create_subsystem nqn.2026-03.io.spdk:null0 -a -s SPDK00000000000001
scripts/rpc.py nvmf_subsystem_add_ns nqn.2026-03.io.spdk:null0 Null0
scripts/rpc.py nvmf_subsystem_add_listener nqn.2026-03.io.spdk:null0 -t tcp -a 10.42.0.1 -s 4420
```

Initiator side:

```bash
build/bin/spdk_nvme_identify -S vcl -r 'trtype:TCP adrfam:IPv4 traddr:10.42.0.1 trsvcid:4420 subnqn:nqn.2026-03.io.spdk:null0'
```

## Notes

- The `vcl` backend is Linux only.
- SPDK still uses its standard socket abstraction. The VCL support is exposed as an additional socket module, not as a separate transport type.
- CPU placement, NUMA locality, VPP worker assignment, and MTU settings remain deployment-specific and should be tuned for the target platform.
