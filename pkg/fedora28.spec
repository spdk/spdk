Name: spdk
Version: 18.07
Release: 3%{?dist}
Epoch: 0
URL: http://spdk.io

Source: https://github.com/spdk/spdk/archive/v%{version}.tar.gz
Summary: Set of libraries and utilities for high performance user-mode storage

# FIXME: Which licence ?
License: BSD

# Only x86_64 is supported
ExclusiveArch: x86_64

BuildRequires: gcc gcc-c++ make
BuildRequires: dpdk-devel, numactl-devel
BuildRequires: libiscsi-devel, libaio-devel, openssl-devel, libuuid-devel
BuildRequires: libibverbs-devel, librdmacm-devel

# Install dependencies
# TODO: check if need to add '>= 17.11'
Requires: dpdk, numactl-libs, openssl-libs
Requires: libiscsi, libaio, libuuid
# NVMe over Fabrics
Requires: librdmacm, librdmacm
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
The Storage Performance Development Kit provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications.


%package devel
Summary: Storage Performance Development Kit development files
Requires: %{name}%{?_isa} = %{epoch}:%{version}-%{release} python3
Provides: %{name}-static = %{epoch}:%{version}-%{release}

%description devel
This package contains the headers and other files needed for
developing applications with the Storage Performance Development Kit.


%package tools
Summary: Storage Performance Development Kit tools files
Requires: %{name}%{?_isa} = %{epoch}:%{version}-%{release} python3

%description tools
Storage Performance Development Kit tools files
<< FIXME: Some more summary ?>>


%prep
# add -q
%autosetup -n spdk-%{version}

%build
./configure --prefix=%{_usr} \
	--disable-tests \
	--without-crypto \
	--with-dpdk=%{getenv:RTE_SDK}/x86_64-default-linuxapp-gcc \
	--without-fio \
	--with-vhost \
	--without-pmdk \
	--without-vpp \
	--without-rbd \
	--with-rdma \
	--with-shared \
	--with-iscsi-initiator \
	--without-raid \
	--without-vtune

%install
%make_install -j`nproc` prefix=%{_usr} libdir=%{_libdir}

# Tools are not yet part of 'make install'
# TODO rename these file PLS!!!
mkdir -p %{buildroot}/%{_datadir}/%{name}/scripts %{buildroot}/%{_sbindir}
for f in rpc.py rpc spdkcli.py spdkcli; do
	cp -r scripts/$f %{buildroot}/%{_datadir}/%{name}/scripts/$f
done

ln -sf -r %{buildroot}/%{_datadir}/%{name}/scripts/rpc.py %{buildroot}/%{_sbindir}/spdk-rpc
ln -sf -r %{buildroot}/%{_datadir}/%{name}/scripts/spdkcli.py %{buildroot}/%{_sbindir}/spdk-cli

# env is banned - replace '/usr/bin/env anything' with '/usr/bin/anything'
sed -i -E '1s@#!/usr/bin/env (.*)@#!/usr/bin/\1@' %{buildroot}/%{_datadir}/%{name}/scripts/*.*

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%{_bindir}/spdk_tgt
# TODO: check if spdk app are static or dynamic linked.
%{_libdir}/*.so.*


%files devel
%{_includedir}/%{name}
%{_libdir}/*.a
%{_libdir}/*.so

%files tools
%{_datadir}/%{name}/scripts
%{_sbindir}/*


%clean
# FIXME: remove this at the end.
echo "NOP"

%changelog
* Tue Sep 18 2018 Pawel Wodkowski <pawelx.wodkowski@intel.com> - 0:18.07-3
- Initial RPM release
