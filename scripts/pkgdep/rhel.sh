#!/usr/bin/env bash

disclaimer() {
	case "$ID" in
		rhel)
			cat <<- WARN

				WARNING: $PRETTY_NAME system detected.

				Please, note that the support for this platform is considered to be "best-effort",
				as in, access to some packages may be limited and/or missing. Review your repo
				setup to make sure installation of all dependencies is possible.

			WARN

			# Don't trigger errexit, simply install what's available. This is default
			# behavior of older yum versions (e.g. the one present on RHEL 7.x) anyway.
			yum() { "$(type -P yum)" --skip-broken "$@"; }
			# For systems which are not registered, subscription-manager will most likely
			# fail on most calls so simply ignore its failures.
			sub() { subscription-manager "$@" || :; }
			;;

		*) ;;
	esac
}

disclaimer

# First, add extra EPEL, ELRepo, Ceph repos to have a chance of covering most of the packages
# on the enterprise systems, like RHEL.
if [[ $ID == centos || $ID == rhel ]]; then
	repos=() enable=("epel" "elrepo" "elrepo-testing")
	[[ $ID == centos ]] && enable+=("extras")
	if [[ $VERSION_ID == 7* ]]; then
		repos+=("https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm")
		repos+=("https://www.elrepo.org/elrepo-release-7.el7.elrepo.noarch.rpm")
		[[ $ID == centos ]] && repos+=("centos-release-ceph-nautilus.noarch")
		# Disable liburing, see https://github.com/spdk/spdk/issues/1564
		if [[ $INSTALL_LIBURING == true ]]; then
			echo "Liburing not supported on ${ID}$VERSION_ID, disabling"
			INSTALL_LIBURING=false
		fi
	fi
	if [[ $VERSION_ID == 8* ]]; then
		repos+=("https://dl.fedoraproject.org/pub/epel/epel-release-latest-8.noarch.rpm")
		repos+=("https://www.elrepo.org/elrepo-release-8.el8.elrepo.noarch.rpm")
		[[ $ID == centos ]] && repos+=("centos-release-ceph-nautilus.noarch")
		# Add PowerTools needed for install CUnit-devel in Centos8
		[[ $ID == centos ]] && enable+=("PowerTools")
	fi
	if ((${#repos[@]} > 0)); then
		yum install -y "${repos[@]}" yum-utils
		yum-config-manager --enable "${enable[@]}"
	fi
	# Potential dependencies can be needed from other RHEL repos, enable them
	if [[ $ID == rhel ]]; then
		[[ $VERSION_ID == 7* ]] && sub repos --enable "rhel-*-optional-rpms" --enable "rhel-*-extras-rpms"
		[[ $VERSION_ID == 8* ]] && sub repos --enable codeready-builder-for-rhel-8-x86_64-rpms
	fi
fi

# Minimal install
# workaround for arm: ninja fails with dep on skbuild python module
if [ "$(uname -m)" = "aarch64" ]; then
	pip3 install scikit-build
	if echo "$ID $VERSION_ID" | grep -E -q 'centos 7'; then
		# by default centos 7.x uses cmake 2.8 while ninja requires 3.6 or higher
		yum install -y cmake3
		# cmake3 is installed as /usr/bin/cmake3 while ninja directly calls `cmake`. Create a soft link
		# as a workaround
		mkdir -p /tmp/bin/
		ln -s /usr/bin/cmake3 /tmp/bin/cmake > /dev/null 2>&1 || true
		export PATH=/tmp/bin:$PATH
	fi
fi

yum install -y gcc gcc-c++ make cmake CUnit-devel libaio-devel openssl-devel \
	libuuid-devel libiscsi-devel ncurses-devel
if echo "$ID $VERSION_ID" | grep -E -q 'centos 8|rhel 8'; then
	yum install -y python36
	#Create hard link to use in SPDK as python
	if [[ ! -e /usr/bin/python && -e /etc/alternative/python3 ]]; then
		ln -s /etc/alternatives/python3 /usr/bin/python
	fi
else
	yum install -y python
fi
yum install -y python3-pip
pip3 install ninja
pip3 install meson

# Additional dependencies for SPDK CLI - not available in rhel and centos
if ! echo "$ID $VERSION_ID" | grep -E -q 'rhel 7|centos 7'; then
	yum install -y python3-configshell python3-pexpect
fi
# Additional dependencies for ISA-L used in compression
yum install -y autoconf automake libtool help2man
# Additional dependencies for DPDK
yum install -y numactl-devel nasm
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	if echo "$ID $VERSION_ID" | grep -E -q 'centos 8'; then
		yum install -y python3-pycodestyle
		echo "Centos 8 does not have lcov and ShellCheck dependencies"
	else
		yum install -y python-pycodestyle lcov ShellCheck
	fi
	yum install -y git astyle sg3_utils pciutils libabigail
fi
if [[ $INSTALL_PMEM == "true" ]]; then
	# Additional dependencies for building pmem based backends
	yum install -y libpmemblk-devel || true
fi
if [[ $INSTALL_FUSE == "true" ]]; then
	# Additional dependencies for FUSE and NVMe-CUSE
	yum install -y fuse3-devel
fi
if [[ $INSTALL_RDMA == "true" ]]; then
	# Additional dependencies for RDMA transport in NVMe over Fabrics
	yum install -y libibverbs-devel librdmacm-devel
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	yum install -y mscgen || echo "Warning: couldn't install mscgen via yum. Please install mscgen manually."
	yum install -y doxygen graphviz
fi
