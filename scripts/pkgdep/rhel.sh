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
			;;

		*) ;;
	esac
}

disclaimer

# First, add extra EPEL repo to have a chance of covering most of the packages
# on the enterprise systems, like RHEL.
if [[ $ID == centos || $ID == rhel ]]; then
	if ! rpm --quiet -q epel-release; then
		[[ $VERSION_ID == 7* ]] && epel=https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm
		[[ $VERSION_ID == 8* ]] && epel=https://dl.fedoraproject.org/pub/epel/epel-release-latest-8.noarch.rpm
		if [[ -n $epel ]]; then
			yum install -y "$epel"
		fi
	fi
	# Potential dependencies for EPEL packages can be needed from other repos, enable them
	if [[ $ID == rhel ]]; then
		[[ $VERSION_ID == 7* ]] && subscription-manager repos --enable "rhel-*-optional-rpms" --enable "rhel-*-extras-rpms"
		[[ $VERSION_ID == 8* ]] && subscription-manager repos --enable codeready-builder-for-rhel-8-x86_64-rpms
	elif [[ $ID == centos ]]; then
		yum --enablerepo=extras install -y epel-release
	fi
fi

# Minimal install
if echo "$ID $VERSION_ID" | grep -E -q 'centos 8'; then
	# Add PowerTools needed for install CUnit-devel in Centos8
	yum install -y yum-utils
	yum config-manager --set-enabled PowerTools
fi
yum install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
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
