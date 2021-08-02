# RPMs {#rpms}

## In this document {#rpms_toc}

* @ref building_rpms

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
