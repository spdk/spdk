Name: spdk
Version: 18.07
Release: 3%{?dist}
Epoch: 0
URL: http://spdk.io

Source: https://github.com/spdk/spdk/archive/v%{version}.tar.gz
Summary: Set of libraries and utilities for high performance user-mode storage

%define package_version %{epoch}:%{version}-%{release}
%define datadir %{buildroot}/%{_datadir}/%{name}
%define sbindir %{buildroot}/%{_sbindir}

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


%package examples
Summary: Storage Performance Development Kit example applications
Requires: %{name}%{?_isa} = %{package_version}
Requires: %{name}-devel%{?_isa} = %{package_version}

%description examples
This package contains example applications for Storage Performance Development Kit.


%package devel
Summary: Storage Performance Development Kit development files
Requires: %{name}%{?_isa} = %{package_version}
Provides: %{name}-static%{?_isa} = %{package_version}


%description devel
This package contains the headers and other files needed for
developing applications with the Storage Performance Development Kit.


%package tools
Summary: Storage Performance Development Kit tools files
Requires: %{name}%{?_isa} = %{package_version} python3
BuildArch: noarch

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
%make_install -j`nproc` prefix=%{_usr} libdir=%{_libdir} datadir=%{_datadir}

# Install tools
mkdir -p %{datadir}
find scripts -type f -regextype egrep -regex '.*(spdkcli|rpc).*[.]py' \
	-exec cp --parents -t %{datadir} {} ";"

# env is banned - replace '/usr/bin/env anything' with '/usr/bin/anything'
find %{datadir}/scripts -type f -regextype egrep -regex '.*([.]py|[.]sh)' \
	-exec sed -i -E '1s@#!/usr/bin/env (.*)@#!/usr/bin/\1@' {} +

# synlinks to tools
mkdir -p %{sbindir}
ln -sf -r %{datadir}/scripts/rpc.py %{sbindir}/%{name}-rpc
ln -sf -r %{datadir}/scripts/spdkcli.py %{sbindir}/%{name}-cli

# Install examples build scripts for examples
cp -r --parents -t %{datadir} examples mk

for ex_app_path in $(find ./examples -executable -type f); do
	cp  $ex_app_path %{sbindir}/%{name}_example_$(basename $ex_app_path)
done
make -C %{datadir}/examples clean


%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig


%files
%{_bindir}/spdk_tgt
%{_libdir}/*.so.*


%files examples
%{_datadir}/%{name}/examples
%{_datadir}/%{name}/mk
%{_sbindir}/%{name}_example_*


%files devel
%{_includedir}/%{name}
%{_libdir}/*.a
%{_libdir}/*.so


%files tools
%{_datadir}/%{name}/scripts
%{_sbindir}/%{name}-rpc
%{_sbindir}/%{name}-cli

%clean
echo Clean NOPE

%changelog
* Tue Sep 18 2018 Pawel Wodkowski <pawelx.wodkowski@intel.com> - 0:18.07-3
- Initial RPM release
