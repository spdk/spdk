# Linking SPDK applications with pkg-config {#pkgconfig}

The SPDK build system generates pkg-config files to facilitate linking
applications with the correct set of SPDK and DPDK libraries. Using pkg-config
in your build system will ensure you do not need to make modifications
when SPDK adds or modifies library dependencies.

If your application is using the SPDK nvme library, you would use the following
to get the list of required SPDK libraries:

~~~bash
PKG_CONFIG_PATH=/path/to/spdk/build/lib/pkgconfig pkg-config --libs spdk_nvme
~~~

To get the list of required SPDK and DPDK libraries to use the DPDK-based
environment layer:

~~~bash
PKG_CONFIG_PATH=/path/to/spdk/build/lib/pkgconfig pkg-config --libs spdk_env_dpdk
~~~

When linking with static libraries, the dependent system libraries must also be
specified. To get the list of required system libraries:

~~~bash
PKG_CONFIG_PATH=/path/to/spdk/build/lib/pkgconfig pkg-config --libs spdk_syslibs
~~~

Note that SPDK libraries use constructor functions liberally, so you must surround
the library list with extra linker options to ensure these functions are not dropped
from the resulting application binary. With shared libraries this is achieved through
the `-Wl,--no-as-needed` parameters while with static libraries `-Wl,--whole-archive`
is used. Here is an example Makefile snippet that shows how to use pkg-config to link
an application that uses the SPDK nvme shared library:

~~~bash
PKG_CONFIG_PATH = $(SPDK_DIR)/build/lib/pkgconfig
SPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_nvme
DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_env_dpdk

app:
	$(CC) -o app app.o -pthread -Wl,--no-as-needed $(SPDK_LIB) $(DPDK_LIB) -Wl,--as-needed
~~~

If using the SPDK nvme static library:

~~~bash
PKG_CONFIG_PATH = $(SPDK_DIR)/build/lib/pkgconfig
SPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_nvme
DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_env_dpdk
SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs

app:
	$(CC) -o app app.o -pthread -Wl,--whole-archive $(SPDK_LIB) $(DPDK_LIB) -Wl,--no-whole-archive \
		$(SYS_LIB)
~~~
