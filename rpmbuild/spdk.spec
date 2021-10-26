# Global macros
%define debug_package %{nil}

%{!?deps:%define deps 1}
%{!?dpdk:%define dpdk 0}
%{!?dpdk_build_path:%define dpdk_build_path "dpdk/build"}
%{!?dpdk_path:%define dpdk_path "dpdk"}
%{!?requirements:%define requirements 0}
%{!?build_requirements:%define build_requirements 0}
%{!?shared:%define shared 0}

# Spec metadata
Name:           spdk
Version:        %{version}
Release:        %{release}
Summary:        Storage Performance Development Kit

# This is a minimal set of requirements needed for SPDK apps to run when built with
# default configuration. These are also predetermined by rpmbuild. Extra requirements
# can be defined through a comma-separated list passed via $requirements when building
# the spec.
Requires: glibc
Requires: libaio
Requires: libgcc
Requires: libstdc++
Requires: libuuid
Requires: ncurses-libs
Requires: numactl-libs
Requires: openssl-libs
Requires: zlib

%if %{requirements}
Requires: %(echo "%{requirements_list}")
%endif

%if %{build_requirements}
BuildRequires: %(echo "%{build_requirements_list}")
%endif

License:       BSD
URL:           https://spdk.io
Source:        spdk-%{version}.tar.gz

%description

The Storage Performance Development Kit (SPDK) provides a set of tools and libraries for
writing high performance, scalable, user-mode storage applications. It achieves high
performance by moving all of the necessary drivers into userspace and operating in a
polled mode instead of relying on interrupts, which avoids kernel context switches and
eliminates interrupt handling overhead.

%prep
make clean &>/dev/null || :
%setup

%build
%if %{deps}
./scripts/pkgdep.sh --docs --pmem --rdma --uring
%endif

# Rely mainly on CONFIG
./configure --disable-unit-tests --disable-tests %{configure}
make %{make}
make DESTDIR=%{buildroot} install

# Include DPDK libs in case --with-shared is in use.
%if %{dpdk}
mkdir -p %{buildroot}/usr/local/lib/dpdk
cp -a %{dpdk_build_path}/lib/* %{buildroot}/usr/local/lib/dpdk/
# Special case for SPDK_RUN_EXTERNAL_DPDK setup
[[ -e %{dpdk_path}/intel-ipsec-mb ]] && find %{dpdk_path}/intel-ipsec-mb/ -name '*.so*' -exec cp -a {} %{buildroot}/usr/local/lib/dpdk/ ';'
[[ -e %{dpdk_path}/isa-l/build/lib ]] && cp -a %{dpdk_path}/isa-l/build/lib/*.so* %{buildroot}/usr/local/lib/dpdk/
%endif

# Try to include all the binaries that were potentially built
[[ -e build/examples ]] && cp -a build/examples/* %{buildroot}/usr/local/bin/
[[ -e build/bin ]] && cp -a build/bin/* %{buildroot}/usr/local/bin/
[[ -e build/fio ]] && cp -a build/fio %{buildroot}/usr/local/bin/fio

# And some useful setup scripts SPDK uses
mkdir -p %{buildroot}/usr/libexec/spdk
mkdir -p %{buildroot}/etc/bash_completion.d
mkdir -p %{buildroot}/etc/profile.d
mkdir -p %{buildroot}/etc/ld.so.conf.d

cat <<-EOF > %{buildroot}/etc/ld.so.conf.d/spdk.conf
/usr/local/lib
/usr/local/lib/dpdk
EOF

cat <<-'EOF' > %{buildroot}/etc/profile.d/spdk_path.sh
PATH=$PATH:/usr/libexec/spdk/scripts
PATH=$PATH:/usr/libexec/spdk/scripts/vagrant
PATH=$PATH:/usr/libexec/spdk/test/common/config
export PATH
EOF

cp -a scripts %{buildroot}/usr/libexec/spdk/scripts
ln -s /usr/libexec/spdk/scripts/bash-completion/spdk %{buildroot}/etc/bash_completion.d/

# We need to take into the account the fact that most of the scripts depend on being
# run directly from the repo. To workaround it, create common root space under dir
# like /usr/libexec/spdk and link all potential relative paths the script may try
# to reference.

# setup.sh uses pci_ids.h
ln -s /usr/local/include %{buildroot}/usr/libexec/spdk

%files
/etc/profile.d/*
/etc/bash_completion.d/*
/usr/libexec/spdk/*
/usr/local/bin/*


%package devel
Summary: SPDK development libraries and headers

%description devel
SPDK development libraries and headers

%files devel
/usr/local/include/*
%if %{shared}
/usr/local/lib/lib*.so
%endif

%package libs
Summary:  SPDK libraries

%description libs
SPDK libraries

%files libs
/etc/ld.so.conf.d/*
/usr/local/lib/lib*.a
/usr/local/lib/pkgconfig/*.pc
%if %{shared}
/usr/local/lib/lib*.so.*
%endif

%post libs
ldconfig

%if %{dpdk}
%package dpdk-libs
Summary: DPDK libraries

%description dpdk-libs
DPDK libraries

%files dpdk-libs
/usr/local/lib/dpdk

%post dpdk-libs
ldconfig
%endif

%changelog
* Tue Feb 16 2021 Michal Berger <michalx.berger@intel.com>
- Initial RPM .spec for the SPDK
