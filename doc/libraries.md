# SPDK Libraries {#libraries}

The SPDK repository is, first and foremost, a collection of high-performance
storage-centric software libraries. With this in mind, much care has been taken
to ensure that these libraries have consistent and robust naming and versioning
conventions. The libraries themselves are also divided across two directories
(`lib` and `module`) inside of the SPDK repository in a deliberate way to prevent
mixing of SPDK event framework dependent code and lower level libraries. This document
is aimed at explaining the structure, naming conventions, versioning scheme, and use cases
of the libraries contained in these two directories.

# Directory Structure {#structure}

The SPDK libraries are divided into two directories. The `lib` directory contains the base libraries that
compose SPDK. Some of these base libraries define plug-in systems. Instances of those plug-ins are called
modules and are located in the `module` directory. For example, the `spdk_sock` library is contained in the
`lib` directory while the implementations of socket abstractions, `sock_posix` and `sock_uring`
are contained in the `module` directory.

## lib {#lib}

The libraries in the `lib` directory can be readily divided into four categories:

- Utility Libraries: These libraries contain basic, commonly used functions that make more complex
libraries easier to implement. For example, `spdk_log` contains macro definitions that provide a
consistent logging paradigm and `spdk_json` is a general purpose JSON parsing library.
- Protocol Libraries: These libraries contain the building blocks for a specific service. For example,
`spdk_nvmf` and `spdk_vhost` each define the storage protocols after which they are named.
- Storage Service Libraries: These libraries provide a specific abstraction that can be mapped to somewhere
between the physical drive and the filesystem level of your typical storage stack. For example `spdk_bdev`
provides a general block device abstraction layer, `spdk_lvol` provides a logical volume abstraction,
`spdk_blobfs` provides a filesystem abstraction, and `spdk_ftl` provides a flash translation layer
abstraction.
- System Libraries: These libraries provide system level services such as a JSON based RPC service
(see `spdk_jsonrpc`) and thread abstractions (see `spdk_thread`). The most notable library in this category
is the `spdk_env_dpdk` library which provides a shim for the underlying Data Plane Development Kit (DPDK)
environment and provides services like memory management.

The one library in the `lib` directory that doesn't fit into the above classification is the `spdk_event` library.
This library defines a framework used by the applications contained in the `app` and `example` directories. Much
care has been taken to keep the SPDK libraries independent from this framework. The libraries in `lib` are engineered
to allow plugging directly into independent application frameworks such as Seastar or libuv with minimal effort.

Currently there are two exceptions in the `lib` directory which still rely on `spdk_event`, `spdk_vhost` and `spdk_iscsi`.
There are efforts underway to remove all remaining dependencies these libraries have on the `spdk_event` library.

Much like the `spdk_event` library, the `spdk_env_dpdk` library has been architected in such a way that it
can be readily replaced by an alternate environment shim. More information on replacing the `spdk_env_dpdk`
module and the underlying `dpdk` environment can be found in the [environment](#env_replacement) section.

## module {#module}

The component libraries in the `module` directory represent specific implementations of the base libraries in
the `lib` directory. As with the `lib` directory, much care has been taken to avoid dependencies on the
`spdk_event` framework except for those libraries which directly implement the `spdk_event` module plugin system.

There are seven sub-directories in the `module` directory which each hold a different class of libraries. These
sub-directories can be divided into two types.

- plug-in libraries: These libraries are explicitly tied to one of the libraries in the `lib` directory and
are registered with that library at runtime by way of a specific constructor function. The parent library in
the `lib` directory then manages the module directly. These types of libraries each implement a function table
defined by their parent library. The following table shows these directories and their corresponding parent
libraries:

<center>
| module directory | parent library | dependent on event library |
|------------------|----------------|----------------------------|
| module/accel     | spdk_accel     | no                         |
| module/bdev      | spdk_bdev      | no                         |
| module/event     | spdk_event     | yes                        |
| module/sock      | spdk_sock      | no                         |
</center>

- Free libraries: These libraries are highly dependent upon a library in the `lib` directory but are not
explicitly registered to that library via a constructor. The libraries in the `blob`, `blobfs`, and `env_dpdk`
directories fall into this category. None of the libraries in this category depend explicitly on the
`spdk_event` library.

# Library Conventions {#conventions}

The SPDK libraries follow strict conventions for naming functions, logging, versioning, and header files.

## Headers {#headers}

All public SPDK header files exist in the `include` directory of the SPDK repository. These headers
are divided into two sub-directories.

`include/spdk` contains headers intended to be used by consumers of the SPDK libraries. All of the
functions, variables, and types in these functions are intended for public consumption. Multiple headers
in this directory may depend upon the same underlying library and work together to expose different facets
of the library. The `spdk_bdev` library, for example, is exposed in three different headers. `bdev_module.h`
defines the interfaces a bdev module library would need to implement, `bdev.h` contains general block device
functions that would be used by an application consuming block devices exposed by SPDK, and `bdev_zone.h`
exposes zoned bdev specific functions. Many of the other libraries exhibit a similar behavior of splitting
headers between consumers of the library and those wishing to register a module with that library.

`include/spdk_internal`, as its name suggests contains header files intended to be consumed only by other
libraries inside of the SPDK repository. These headers are typically used for sharing lower level functions
between two libraries that both require similar functions. For example `spdk_internal/nvme_tcp.h` contains
low level tcp functions used by both the `spdk_nvme` and `spdk_nvmf` libraries. These headers are *NOT*
intended for general consumption.

Other header files contained directly in the `lib` and `module` directories are intended to be consumed *only*
by source files of their corresponding library. Any symbols intended to be used across libraries need to be
included in a header in the `include/spdk_internal` directory.

## Naming Conventions {#naming}

All public types and functions in SPDK libraries begin with the prefix `spdk_`. They are also typically
further namespaced using the spdk library name. The rest of the function or type name describes its purpose.

There are no internal library functions that begin with the `spdk_` prefix. This naming convention is
enforced by the SPDK continuous Integration testing. Functions not intended for use outside of their home
library should be namespaced with the name of the library only.

## Map Files {#map}

SPDK libraries can be built as both static and shared object files. To facilitate building libraries as shared
objects, each one has a corresponding map file (e.g. `spdk_nvmf` relies on `spdk_nvmf.map`). SPDK libraries
not exporting any symbols rely on a blank map file located at `mk/spdk_blank.map`.

# SPDK Shared Objects {#shared_objects}

## Shared Object Versioning {#versioning}

SPDK shared objects follow a semantic versioning pattern with a major and minor version. Any changes which
break backwards compatibility (symbol removal or change) will cause a shared object major increment and
backwards compatible changes will cause a minor version increment; i.e. an application that relies on
`libspdk_nvmf.so.3.0` will be compatible with `libspdk_nvmf.so.3.1` but not with `libspdk_nvmf.so.4.0`.

Shared object versions are incremented only once between each release cycle. This means that at most, the
major version of each SPDK shared library will increment only once between each SPDK release.

There are currently no guarantees in SPDK of ABI compatibility between two major SPDK releases.

The point releases of an LTS release will be ABI compatible with the corresponding LTS major release.

Shared objects are versioned independently of one another. This means that `libspdk_nvme.so.3.0` and
`libspdk_bdev.so.3.0` do not necessarily belong to the same release. This also means that shared objects
with the same suffix are not necessarily compatible with each other. It is important to source all of your
SPDK libraries from the same repository and version to ensure inter-library compatibility.

## Linking to Shared Objects {#so_linking}

Shared objects in SPDK are created on a per-library basis. There is a top level `libspdk.so` object
which is a linker script. It simply contains references to all of the other spdk shared objects.

There are essentially two ways of linking to SPDK libraries.

1. An application can link to the top level shared object library as follows:
~~~{.sh}
	gcc -o my_app ./my_app.c -lspdk -lspdk_env_dpdk -ldpdk
~~~

2. An application can link to only a subset of libraries by linking directly to the ones it relies on:
~~~{.sh}
	gcc -o my_app ./my_app.c -lpassthru_external -lspdk_event_bdev -lspdk_bdev -lspdk_bdev_malloc
	-lspdk_log -lspdk_thread -lspdk_util -lspdk_event -lspdk_env_dpdk -ldpdk
~~~

In the second instance, please note that applications need only link to the libraries upon which they
directly depend. All SPDK libraries have their dependencies specified at object compile time. This means
that when linking to `spdk_net`, one does not also have to specify `spdk_log`, `spdk_util`, `spdk_json`,
`spdk_jsonrpc`, and `spdk_rpc`. However, this dependency inclusion does not extend to the application
itself; i.e. if an application directly uses symbols from both `spdk_bdev` and `spdk_log`, both libraries
will need to be supplied to the linker when linking the application even though `spdk_log` is a dependency
of `spdk_bdev`.

Please also note that when linking to SPDK libraries, both the spdk_env shim library and the env library
itself need to be supplied to the linker. In the examples above, these are `spdk_env_dpdk` and `dpdk`
respectively. This was intentional and allows one to easily swap out both the environment and the
environment shim.

## Replacing the env abstraction {#env_replacement}

SPDK depends on an environment abstraction that provides crucial pinned memory management and PCIe
bus management operations. The interface for this environment abstraction is defined in the
`include/env.h` header file. The default implementation of this environment is located in `spdk_env_dpdk`.
This abstraction in turn relies upon the DPDK libraries. This two part implementation was deliberate
and allows for easily swapping out the dpdk version upon which the spdk libraries rely without making
modifications to the spdk source directly.

Any environment can replace the `spdk_env_dpdk` environment by implementing the `include/env.h` header
file. The environment can either be implemented wholesale in a single library or as a two-part
shim/implementation library system.
~~~{.sh}
	# single library
	gcc -o my_app ./my_app.c -lspdk -lcustom_env_implementation

	# two libraries
	gcc -o my_app ./my_app.c -lspdk -lcustom_env_shim -lcustom_env_implementation
~~~
