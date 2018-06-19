#!/usr/bin/env bash

if [ ! "$USER" = "root" ]; then
        echo
        echo Error: must be run as root!
        echo
        exit 1
fi

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SPDK_DIR="$( cd "${DIR}/../../" && pwd )"
echo "SPDK_DIR = $SPDK_DIR"

# Bug fix for vagrant rsync problem
if [ -d /home/vagrant/spdk_repo ]; then
	chown vagrant /home/vagrant/spdk_repo
	chgrp vagrant /home/vagrant/spdk_repo
fi

SYSTEM=`uname -s`

if [ "$SYSTEM" = "FreeBSD" ]; then
	# Do initial setup for the system
	${SPDK_DIR}/scripts/pkgdep.sh
	git clone https://github.com/freebsd/freebsd.git -b release/11.1.0 /usr/src
else

	# Make sure that we get the hugepages we need on provision boot
	# Note: The package install should take care of this at the end
	#       But sometimes after all the work of provisioning, we can't
	#       get the requested number of hugepages without rebooting.
	#       So do it here just in case
	sysctl -w vm.nr_hugepages=1024
	HUGEPAGES=`sysctl -n  vm.nr_hugepages`
	if [ $HUGEPAGES != 1024 ]; then
		echo "ERROR: Unable to get 1024 hugepages, only got $HUGEPAGES.  Cannot finish."
		exit
	fi

	# Figure out what system we are running on
	if [ -f /etc/lsb-release ];then
		. /etc/lsb-release
	elif [ -f /etc/redhat-release ];then
		yum update -y
		yum install -y redhat-lsb
		DISTRIB_ID=`lsb_release -si`
		DISTRIB_RELEASE=`lsb_release -sr`
		DISTRIB_CODENAME=`lsb_release -sc`
		DISTRIB_DESCRIPTION=`lsb_release -sd`
	fi

	# Do initial setup for the system
	if [ "$DISTRIB_ID" == "Ubuntu" ]; then
		export DEBIAN_PRIORITY=critical
		export DEBIAN_FRONTEND=noninteractive
		export DEBCONF_NONINTERACTIVE_SEEN=true
		APT_OPTS="--assume-yes --no-install-suggests --no-install-recommends -o Dpkg::Options::=\"--force-confdef\" -o Dpkg::Options::=\"--force-confold\""
		# Standard update + upgrade dance
		apt-get update ${APT_OPTS}
		#apt-get upgrade ${APT_OPTS}
		${SPDK_DIR}/scripts/pkgdep.sh
	elif [ "$DISTRIB_ID" == "CentOS" ]; then
		# Standard update + upgrade dance
		yum check-update
		yum update -y
		${SPDK_DIR}/scripts/pkgdep.sh
	elif [ "$DISTRIB_ID" == "CentOS" ]; then
		yum check-update
		yum update -y
		${SPDK_DIR}/scripts/pkgdep.sh
		if [ "$DISTRIB_RELEASE" = "26" ]; then
			echo
			echo "  Run \"${SPDK_DIR}/test/common/config/vm_setup.sh\" to complete setup of Fedora 26"
			echo
		fi
	fi
fi
