#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

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

cd /opt/stack/devstack
su -c "./unstack.sh" -s /bin/bash stack

cd /opt/stack
rm -rf cinder devstack glance keystone heat horizon neutron nova placement requirements tacker tacker-horizon tempest

r=0
until [[ $r -ge 20 ]]; do
	if [[ $branch == "master" ]]; then
		su -c "git clone --depth 1 https://opendev.org/openstack-dev/devstack" -s /bin/bash stack && break
	else
		su -c "git clone --depth 1 https://opendev.org/openstack-dev/devstack -b stable/$branch" -s /bin/bash stack && break
	fi
	r=$((r + 1))
done
cp $rootdir/scripts/vagrant/local.conf /opt/stack/devstack/local.conf

cd /opt/stack/devstack
sudo sed -i "s|http://download.cirros-cloud.net|https://download.cirros-cloud.net|g" stackrc
su -c "./stack.sh" -s /bin/bash stack
source openrc admin admin
openstack volume type create SPDK --public
