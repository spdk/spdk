# RPMs {#rpms}

## In this document {#rpms_toc}

* @ref building_rpms
* @ref dpdk_devel

## Building SPDK RPMs {#building_rpms}

To build basic set of RPM packages out of the SPDK repo simply run:

~~~{.sh}
# rpmbuild/rpm.sh
~~~

Additional configuration options can be passed directly as arguments:

~~~{.sh}
# rpmbuild/rpm.sh --with-shared --with-dpdk=/path/to/dpdk/build
~~~

There are several options that may be passed via environment as well:

- DEPS          - Install all needed dependencies for building RPM packages.
                Default: "yes"
- MAKEFLAGS     - Flags passed to make
- RPM_RELEASE   - Target release version of the RPM packages. Default: 1
- REQUIREMENTS  - Extra set of RPM dependencies if deemed as needed
- SPDK_VERSION  - SPDK version. Default: currently checked out tag
- GEN_SPEC      - Orders rpm.sh to only generate a valid .spec and print
                it on stdout. The content of the .spec is determined based
                mainly on the ./configure cmdline passed to rpm.sh.
- USE_DEFAULT_DIRS - Normally, rpm.sh will order rpmbuild to build under
                   customizable set of directories. Since this may be not
                   desired, especially when used together with GEN_SPEC,
                   this option will preserve the default set of directories.

~~~{.sh}
# DEPS=no MAKEFLAGS="-d -j1" rpmbuild/rpm.sh --with-shared
~~~

By default, all RPM packages should be created under $HOME directory of the
target user:

~~~{.sh}
# printf '%s\n' /root/rpmbuild/RPMS/x86_64/*
/root/rpmbuild/RPMS/x86_64/spdk-devel-v21.01-1.x86_64.rpm
/root/rpmbuild/RPMS/x86_64/spdk-dpdk-libs-v21.01-1.x86_64.rpm
/root/rpmbuild/RPMS/x86_64/spdk-libs-v21.01-1.x86_64.rpm
/root/rpmbuild/RPMS/x86_64/spdk-v21.01-1.x86_64.rpm
#
~~~

- spdk            - provides all the binaries, common tooling, etc.
- spdk-devel      - provides development files
- spdk-libs       - provides target lib, .pc files (--with-shared)
- spdk-dpdk-libs  - provides dpdk lib files (--with-shared|--with-dpdk)

## Special case for dpdk-devel {#dpdk_devel}

When rpm.sh finds a bare --with-dpdk argument on the cmdline it will try to
adjust the behavior of the rpmbuild to make sure only SPDK RPMs are built.
Since this argument requests SPDK to be built against installed DPDK (e.g.
dpdk-devel package) the spdk-dpdk-libs RPM won't be included.  Moreover, the
.spec will be armed with a build requirement to make sure dpdk-devel is
present on the building system. The minimum required version of dpdk-devel
is set to 19.11.
