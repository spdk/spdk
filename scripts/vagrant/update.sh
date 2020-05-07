#!/usr/bin/env bash

if [ ! "$USER" = "root" ]; then
	echo
	echo Error: must be run as root!
	echo
	exit 1
fi

set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPDK_DIR="$(cd "${DIR}/../../" && pwd)"
echo "SPDK_DIR = $SPDK_DIR"

# Bug fix for vagrant rsync problem
if [ -d /home/vagrant/spdk_repo ]; then
	echo "Fixing permissions on /home/vagrant/spdk_repo"
	chown vagrant /home/vagrant/spdk_repo
	chgrp vagrant /home/vagrant/spdk_repo
fi

# Setup for run-autorun.sh
if [ ! -f /home/vagrant/autorun-spdk.conf ]; then
	echo "Copying scripts/vagrant/autorun-spdk.conf to /home/vagrant"
	cp ${SPDK_DIR}/scripts/vagrant/autorun-spdk.conf /home/vagrant
	chown vagrant /home/vagrant/autorun-spdk.conf
	chgrp vagrant /home/vagrant/autorun-spdk.conf
fi

SYSTEM=$(uname -s)

if [ "$SYSTEM" = "FreeBSD" ]; then
	# Do initial setup for the system
	pkg upgrade -f
	${SPDK_DIR}/scripts/pkgdep.sh --all
	if [ -d /usr/src/.git ]; then
		echo
		echo "/usr/src/ is a git repository"
		echo "consider \"cd /usr/src/; git pull\" to update"
		echo
	else
		git clone --depth 10 -b release/11.1.0 https://github.com/freebsd/freebsd.git /usr/src
	fi
else

	# Make sure that we get the hugepages we need on provision boot
	# Note: The package install should take care of this at the end
	#       But sometimes after all the work of provisioning, we can't
	#       get the requested number of hugepages without rebooting.
	#       So do it here just in case
	sysctl -w vm.nr_hugepages=1024
	HUGEPAGES=$(sysctl -n vm.nr_hugepages)
	if [ $HUGEPAGES != 1024 ]; then
		echo "Warning: Unable to get 1024 hugepages, only got $HUGEPAGES"
		echo "Warning: Adjusting HUGEMEM in /home/vagrant/autorun-spdk.conf"
		sed "s/HUGEMEM=.*$/HUGEMEM=${HUGEPAGES}/g" /home/vagrant/autorun-spdk.conf > /home/vagrant/foo.conf
		mv -f /home/vagrant/foo.conf /home/vagrant/autorun-spdk.conf
	fi

	# Figure out what system we are running on
	if [ -f /etc/lsb-release ]; then
		. /etc/lsb-release
	elif [ -f /etc/redhat-release ]; then
		yum update -y
		yum install -y redhat-lsb
		DISTRIB_ID=$(lsb_release -si)
		DISTRIB_RELEASE=$(lsb_release -sr)
		DISTRIB_CODENAME=$(lsb_release -sc)
		DISTRIB_DESCRIPTION=$(lsb_release -sd)
	fi

	# Do initial setup for the system
	if [ "$DISTRIB_ID" == "Ubuntu" ]; then
		set -xv
		export DEBIAN_PRIORITY=critical
		export DEBIAN_FRONTEND=noninteractive
		export DEBCONF_NONINTERACTIVE_SEEN=true
		# Standard update + upgrade dance
		apt-get update --assume-yes --no-install-suggests --no-install-recommends -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold"
		apt-get upgrade --assume-yes --no-install-suggests --no-install-recommends -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold"
		${SPDK_DIR}/scripts/pkgdep.sh --all
		apt-get clean
	elif [ "$DISTRIB_ID" == "CentOS" ]; then
		# Standard update + upgrade dance
		yum check-update
		yum update -y
		${SPDK_DIR}/scripts/pkgdep.sh --all
		yum clean all
	elif [ "$DISTRIB_ID" == "Fedora" ]; then
		yum check-update
		yum update -y
		"$SPDK_DIR"/scripts/pkgdep.sh --all
		sudo -u vagrant "$SPDK_DIR"/test/common/config/vm_setup.sh -i
		yum clean all
	fi
	cat /dev/null > ~/.bash_history && history -c
fi
