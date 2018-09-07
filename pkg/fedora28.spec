Name: spdk
Version: 18.07
Release: 1%{?dist}
Epoch: 0
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
BuildRequires: libaio-devel, openssl-devel, libuuid-devel

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
	--without-rdma \
	--without-iscsi-initiator \
	--without-raid \
	--without-vtune

%install
%make_install -j`nproc` prefix=%{_usr} libdir=%{_libdir}


%files
%{_bindir}/spdk_tgt
%{_libdir}/*.so*


%files devel
%{_includedir}/%{name}
%{_libdir}/*.a

%files tools


%changelog