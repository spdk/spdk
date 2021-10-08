#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source "$rootdir/test/common/autotest_common.sh"

function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Devstack installation script"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo "--branch=BRANCH    Define which version of openstack"
	echo "                   should be installed. Default is master."
	echo "-h, --help         Print help and exit"

	exit 0
}

branch="master"
while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage $0 ;;
				branch=*) branch="${OPTARG#*=}" ;;
			esac
			;;
		h) usage $0 ;;
		*) usage $0 "Invalid argument '$OPTARG'" ;;
	esac
done

if [[ -e /opt/stack/devstack/unstack.sh ]]; then
	cd /opt/stack/devstack
	su -c "./unstack.sh" -s /bin/bash stack
fi

mkdir -p /opt/stack
rm -rf /opt/stack/*

r=0
until ((++r >= 20)); do
	if [[ $branch == "master" ]]; then
		git clone --depth 1 https://opendev.org/openstack-dev/devstack /opt/stack/devstack && break
		# FIXME: Workaround for broken requirements with suds-jurko<->setuptools
		export REQUIREMENTS_BRANCH=stable/xena
	else
		git clone --depth 1 https://opendev.org/openstack-dev/devstack -b "stable/$branch" /opt/stack/devstack && break
	fi
done

# Check if we reached max retries count
((r < 20))

git clone https://github.com/openstack/os-brick.git /opt/stack/os-brick
cd /opt/stack/os-brick
python3 ./setup.py install

cp $rootdir/scripts/vagrant/local.conf /opt/stack/devstack/local.conf

# unset PYTHONPATH set by autotest_common.sh - keystone calls to stevedore's caching api and hits sys.path. In
# our case the list includes $rootdir/scripts which stack user can't access due to lack of permissions.
# Setting FORCE=yes allows stack.sh to run under distro versions which are not included in SUPPORTED_DISTROS.
# This allows us to be more relaxed and run under a bit newer|older versions of the same distro (e.g. ubuntu)
# in our CI.
cd /opt/stack/devstack
./tools/create-stack-user.sh
chown -R stack:stack /opt/stack
su -c "PYTHONPATH= FORCE=yes ./stack.sh" -s /bin/bash stack
source openrc admin admin
openstack volume type create SPDK --public

if [[ $branch == master ]]; then
	# FIXME: For some reason tempest won't work unless neutron has securitygroup enabled
	# (even when testing with security_group disabled in tempest.conf). For the time
	# being, until someone understands why this seem to be the case, patch the ml2 plugin
	# config - instead of touching our local.conf which is still valid for the wallaby -
	# to enable the securitygroup right before the tests start.
	[[ -e /etc/neutron/plugins/ml2/ml2_conf.ini ]]
	sed -i -e "s/enable_security_group = False/enable_security_group = True/g" /etc/neutron/plugins/ml2/ml2_conf.ini
fi
