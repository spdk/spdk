# SPDK Structural Overview {#overview}

# Overview {#dir_overview}

SPDK is composed of a set of C libraries residing in `lib` with public interface
header files in `include/spdk`, plus a set of applications built out of those
libraries in `app`. Users can use the C libraries in their software or deploy
the full SPDK applications.

SPDK is designed around message passing instead of locking, and most of the SPDK
libraries make several assumptions about the underlying threading model of the
application they are embedded into. However, SPDK goes to great lengths to remain
agnostic to the specific message passing, event, co-routine, or light-weight
threading framework actually in use. To accomplish this, all SPDK libraries
interact with an abstraction library in `lib/thread` (public interface at
`include/spdk/thread.h`). Any framework can initialize the threading abstraction
and provide callbacks to implement the functionality that the SPDK libraries
need. For more information on this abstraction, see @ref concurrency.

SPDK is built on top of POSIX for most operations. To make porting to non-POSIX
environments easier, all POSIX headers are isolated into
`include/spdk/stdinc.h`. However, SPDK requires a number of operations that
POSIX does not provide, such as enumerating the PCI devices on the system or
allocating memory that is safe for DMA. These additional operations are all
abstracted in a library called `env` whose public header is at
`include/spdk/env.h`. By default, SPDK implements the `env` interface using a
library based on DPDK. However, that implementation can be swapped out. See @ref
porting for additional information.

## Applications {#dir_app}

The `app` top-level directory contains full-fledged applications, built out of the SPDK
components. For a full overview, see @ref app_overview.

SPDK applications can typically be started with a small number of configuration
options. Full configuration of the applications is then performed using
JSON-RPC. See @ref jsonrpc for additional information.

## Libraries {#dir_lib}

The `lib` directory contains the real heart of SPDK. Each component is a C library with
its own directory under `lib`. Some of the key libraries are:

- @ref bdev
- @ref nvme

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

Most of the headers here correspond with a library in the `lib` directory. There
are a few headers that stand alone, however. They are:

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

## Scripts {#dir_scripts}

The `scripts` directory contains convenient scripts for a number of operations. The two most
important are `check_format.sh`, which will use astyle and pep8 to check C, C++, and Python
coding style against our defined conventions, and `setup.sh` which binds and unbinds devices
from kernel drivers.

## Tests {#dir_tests}

The `test` directory contains all of the tests for SPDK's components and the subdirectories mirror
the structure of the entire repository. The tests are a mixture of unit tests and functional tests.
