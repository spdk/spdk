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

cd /opt/stack/devstack
./tools/create-stack-user.sh
chown -R stack:stack /opt/stack
su -c "./stack.sh" -s /bin/bash stack
source openrc admin admin
openstack volume type create SPDK --public
