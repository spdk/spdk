# Global macros
%define debug_package %{nil}

%{!?deps:%define deps 1}
%{!?dpdk:%define dpdk 0}
%{!?fio:%define fio 0}
%{!?requirements:%define requirements 0}
%{!?build_requirements:%define build_requirements 0}
%{!?shared:%define shared 0}
%{!?rbd:%define rbd 0}
%{!?libdir:%define libdir /usr/local/lib}
%{!?vfio_user:%define vfio_user 0}

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

BuildRequires: python3-devel

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
make clean %{make} &>/dev/null || :
%setup

%build
set +x

cfs() {
	(($# > 1)) || return 0

	local dst=$1 f

	mkdir -p "$dst"
	shift; for f; do [[ -e $f ]] && cp -a "$f" "$dst"; done
}

cl() {
	[[ -e $2 ]] || return 0

	cfs "$1" $(find "$2" -name '*.so*' -type f -o -type l | grep -v .symbols)
}

%if %{deps}
_PKGDEP_OPTS="--docs --pmem --rdma --uring"
%if %{rbd}
_PKGDEP_OPTS="$_PKGDEP_OPTS --rbd"
%endif
./scripts/pkgdep.sh $_PKGDEP_OPTS
%endif

# Rely mainly on CONFIG
git submodule update --init
./configure --disable-unit-tests --disable-tests %{configure}
make %{make}
make DESTDIR=%{buildroot} install %{make}
# DPDK always builds both static and shared, so we need to remove one or the other
# SPDK always builds static, so remove it if we want shared.
%if %{shared}
	rm -f %{buildroot}/usr/local/lib/lib*.a
%endif
%if "%{shared}" != "1"
	rm -f %{buildroot}/usr/local/lib/lib*.so*
	rm -rf %{buildroot}/usr/local/lib/dpdk
%endif
%if %{dpdk}
# DPDK also installs some python scripts to bin that we do not want to package here
rm -f %{buildroot}/usr/local/bin/dpdk-*.py
# DPDK examples do not need to be packaged in our RPMs
rm -rf %{buildroot}/usr/local/share/dpdk
# In case sphinx-build is available, DPDK will leave some files we don't need
rm -rf %{buildroot}/usr/local/share/doc/dpdk
%endif

# The ISA-L install may have installed some binaries that we do not want to package
rm -f %{buildroot}/usr/local/bin/igzip
rm -rf %{buildroot}/usr/local/share/man

# Include libvfio-user libs in case --with-vfio-user is in use together with --with-shared
%if %{vfio_user} && %{shared}
cl %{buildroot}/usr/local/lib/libvfio-user build/libvfio-user/
%endif

# And some useful setup scripts SPDK uses
mkdir -p %{buildroot}/usr/libexec/spdk
mkdir -p %{buildroot}/etc/bash_completion.d
mkdir -p %{buildroot}/etc/profile.d
mkdir -p %{buildroot}/etc/ld.so.conf.d

%if %{shared}
cat <<-EOF > %{buildroot}/etc/ld.so.conf.d/spdk.conf
%{libdir}
/usr/local/lib/dpdk
/usr/local/lib/libvfio-user
EOF
%endif

cat <<-'EOF' > %{buildroot}/etc/profile.d/spdk_path.sh
PATH=$PATH:/usr/libexec/spdk/scripts
PATH=$PATH:/usr/libexec/spdk/scripts/vagrant
PATH=$PATH:/usr/libexec/spdk/test/common/config
export PATH
EOF

cfs %{buildroot}/usr/libexec/spdk scripts
ln -s /usr/libexec/spdk/scripts/bash-completion/spdk %{buildroot}/etc/bash_completion.d/

# We need to take into the account the fact that most of the scripts depend on being
# run directly from the repo. To workaround it, create common root space under dir
# like /usr/libexec/spdk and link all potential relative paths the script may try
# to reference.

# setup.sh uses pci_ids.h
ln -s /usr/local/include %{buildroot}/usr/libexec/spdk

%files
/usr/local/bin/*
/usr/local/lib/python%{python3_version}/site-packages/spdk*/*

%package devel
%if %{shared}
Summary: SPDK development libraries and headers
%endif
%if "%{shared}" != "1"
Summary: SPDK static development libraries and headers
%endif

%description devel
%if %{shared}
SPDK development libraries and header
%endif
%if "%{shared}" != "1"
SPDK static development libraries and header
%endif

%files devel
/usr/local/include/*
%{libdir}/pkgconfig/*.pc
%{libdir}/*.la
%if %{fio}
%{libdir}/fio
%endif
%if %{shared}
%{libdir}/*.so*
/etc/ld.so.conf.d/spdk.conf
%if %{dpdk}
%{libdir}/dpdk
%endif
%if %{vfio_user}
/usr/local/lib/libvfio-user
%endif
%endif
%if "%{shared}" != "1"
%{libdir}/*.a
%endif

%post devel
ldconfig

%package scripts
Summary: SPDK scripts and utilities

%description scripts
SPDK scripts and utilities

%files scripts
/usr/libexec/spdk/*
/etc/profile.d/*
/etc/bash_completion.d/*

%post scripts
ldconfig

%changelog
* Tue Feb 16 2021 Michal Berger <michalx.berger@intel.com>
- Initial RPM .spec for the SPDK
