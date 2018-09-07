Name: spdk
Version: 18.07
Release: 1%{?dist}
Epoch: 1
URL: http://spdk.io

Source: https://github.com/spdk/spdk/archive/v%{version}.tar.gz
# FIXME: some general summary.
Summary: << FIXME: summary here >>

# FIXME: Which licence ?
License: BSD

# Only x86_64 is supported
ExclusiveArch: x86_64

BuildRequires: gcc gcc-c++ make
BuildRequires: dpdk-devel, numactl-devel
BuildRequires: libiscsi-devel, libaio-devel, openssl-devel, libuuid-devel
BuildRequires: libibverbs-devel, librdmacm-devel

# Install dependencies
Requires: dpdk, numactl-libs, openssl-libs
Requires: libiscsi, libaio, libuuid
# NVMe over Fabrics
Requires: librdmacm, librdmacm

%description
The Storage Performance Development Kit provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications.


%package devel
Summary: Storage Performance Development Kit development files
Requires: %{name}%{?_isa} = %{epoch}:%{version}-%{release} python3
Provides: %{name}-static = %{epoch}:%{version}-%{release}

%description devel
Storage Performance Development Kit development files
<< FIXME: Some more summary ? >>


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
	--with-iscsi-initiator \
	--without-raid \
	--without-vtune

%install
%make_install -j`nproc` prefix=%{_usr} libdir=%{_libdir}

# Tools are not yet part of 'make install'
# TODO rename these file PLS!!!
mkdir -p %{buildroot}/%{_datadir}/%{name}/scripts %{buildroot}/%{_sbindir}
for f in rpc.py rpc spdkcli.py spdkcli setup.sh; do
	cp -r scripts/$f %{buildroot}/%{_datadir}/%{name}/scripts/$f
done

pwd
ln -sf -r %{buildroot}/%{_datadir}/%{name}/scripts/rpc.py %{buildroot}/%{_sbindir}/spdk-rpc
ln -sf -r %{buildroot}/%{_datadir}/%{name}/scripts/spdkcli.py %{buildroot}/%{_sbindir}/spdk-cli
ln -sf -r %{buildroot}/%{_datadir}/%{name}/scripts/setup.sh %{buildroot}/%{_sbindir}/spdk-setup

# FIXME: post a patch to use python3
sed -i -E '1s@#!/usr/bin/env python@#!/usr/bin/env python3@' %{buildroot}/%{_datadir}/%{name}/scripts/*.py

# env is banned - replace /usr/bin/env anything with /usr/bin/anything
sed -i -E '1s@#!/usr/bin/env (.*)@#!/usr/bin/\1@' %{buildroot}/%{_datadir}/%{name}/scripts/*.*

%files
%{_bindir}/spdk_tgt
%{_libdir}/*.so


%files devel
%{_includedir}/%{name}
%{_libdir}/*.a


%files tools
%{_datadir}/%{name}/scripts
%{_sbindir}

%clean
# FIXME: remove this at the end.
echo "NOP"

%changelog
