# SPDK Directory Structure {#directory_structure}

# Overview {#dir_overview}

SPDK is primarily a collection of C libraries intended to be consumed directly by
applications, but the repository also contains many examples and full-fledged applications.
This will provide a general overview of what is where in the repository.

## Applications {#dir_app}

The `app` top-level directory contains four applications:
 - `app/iscsi_tgt`: An iSCSI target
 - `app/nvmf_tgt`: An NVMe-oF target
 - `app/iscsi_top`: Informational tool (like `top`) that tracks activity in the
    iSCSI target.
 - `app/trace`: A tool for processing trace points output from the iSCSI and
    NVMe-oF targets.
 - `app/vhost`:  A vhost application that presents virtio controllers to
    QEMU-based VMs and process I/O submitted to those controllers.

The application binaries will be in their respective directories after compiling and all
can be run with no arguments to print out their command line arguments. For the iSCSI
and NVMe-oF targets, they both need a configuration file (-c option). Fully commented
examples of the configuration files live in the `etc/spdk` directory.

## Build Collateral {#dir_build}

The `build` directory contains all of the static libraries constructed during
the build process. The `lib` directory combined with the `include/spdk`
directory are the official outputs of an SPDK release, if it were to be packaged.

## Documentation {#dir_doc}

The `doc` top-level directory contains all of SPDK's documentation. API Documentation
is created using Doxygen directly from the code, but more general articles and longer
explanations reside in this directory, as well as the Doxygen config file.

To build the documentation, just type `make` within the doc directory.

## Examples {#dir_examples}

The `examples` top-level directory contains a set of examples intended to be used
for reference. These are different than the applications, which are doing a "real"
task that could reasonably be deployed. The examples are instead either heavily
contrived to demonstrate some facet of SPDK, or aren't considered complete enough
to warrant tagging them as a full blown SPDK application.

This is a great place to learn about how SPDK works. In particular, check out
`examples/nvme/hello_world`.

## Include {#dir_include}

The `include` directory is where all of the header files are located. The public API
is all placed in the `spdk` subdirectory of `include` and we highly
recommend that applications set their include path to the top level `include`
directory and include the headers by prefixing `spdk/` like this:

~~~{.c}
#include "spdk/nvme.h"
~~~

Most of the headers here correspond with a library in the `lib` directory and will be
covered in that section. There are a few headers that stand alone, however. They are:

 - `assert.h`
 - `barrier.h`
 - `endian.h`
 - `fd.h`
 - `mmio.h`
 - `queue.h` and `queue_extras.h`
 - `string.h`

There is also an `spdk_internal` directory that contains header files widely included
by libraries within SPDK, but that are not part of the public API and would not be
installed on a user's system.

## Libraries {#dir_lib}

The `lib` directory contains the real heart of SPDK. Each component is a C library with
its own directory under `lib`.

### Block Device Abstraction Layer {#dir_bdev}

The `bdev` directory contains a block device abstraction layer that is currently used
within the iSCSI and NVMe-oF targets. The public interface is `include/spdk/bdev.h`.
This library lacks clearly defined responsibilities as of this writing and instead does a
number of
things:
 - Translates from a common `block` protocol to specific protocols like NVMe or to system
  calls like libaio. There are currently three block device backend modules that can be
  plugged in - libaio, SPDK NVMe, CephRBD, and a RAM-based backend called malloc.
 - Provides a mechanism for composing virtual block devices from physical devices (to do
  RAID and the like).
 - Handles some memory allocation for data buffers.

This layer also could be made to do I/O queueing or splitting in a general way. We're open
to design ideas and discussion here.

### Configuration File Parser {#dir_conf}

The `conf` directory contains configuration file parser. The public header
is `include/spdk/conf.h`. The configuration file format is kind of like INI,
except that the directives are are "Name Value" instead of "Name = Value". This is
the configuration format for both the iSCSI and NVMe-oF targets.

... Lots more libraries that need to be described ...

## Makefile Fragments {#dir_mk}

The `mk` directory contains a number of shared Makefile fragments used in the build system.

## Scripts {#dir_scripts}

The `scripts` directory contains convenient scripts for a number of operations. The two most
important are `check_format.sh`, which will use astyle and pep8 to check C, C++, and Python
coding style against our defined conventions, and `setup.sh` which binds and unbinds devices
from kernel drivers.

## Tests {#dir_tests}

The `test` directory contains all of the tests for SPDK's components and the subdirectories mirror
the structure of the entire repository. The tests are a mixture of unit tests and functional tests.
