This directory is meant to demonstrate how to link an external application and bdev
module to the SPDK libraries. The makefiles contain six examples of linking against spdk
libraries. They cover linking an application both with and without a custom bdev. For each of
these categories, they also demonstrate linking against the spdk combined shared library,
individual shared libraries, and static libraries.

This directory also contains a convenient test script, test_make.sh, which automates making SPDK
and testing all six of these linker options. It takes a single argument, the path to an SPDK
repository and should be run as follows:

~~~bash
sudo ./test_make.sh /path/to/spdk
~~~

The application `hello_bdev` is a symlink and bdev module `passthru_external` have been copied from their namesakes
in the top level [SPDK github repository](https://github.com/spdk/spdk) and don't have any special
functionality.
