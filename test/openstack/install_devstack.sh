#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

branch="stein"
case $1 in
	-m|--master)
		branch="master"
		;;
	-s|--stein)
		branch="stein"
		;;
	*)
		echo "unknown branch type: $1"
		exit 1
	;;
esac

cd /opt/stack/devstack
su -c "./unstack.sh" -s /bin/bash stack

cd /opt/stack
rm -rf cinder devstack glance keystone heat horizon neutron nova placement requirements tacker tacker-horizon tempest

if [[ $branch == "master" ]]; then
	su -c "git clone https://opendev.org/openstack-dev/devstack" -s /bin/bash stack
	cp $rootdir/scripts/vagrant/local_master.conf /opt/stack/devstack/local.conf
elif [[ $branch == "stein" ]]; then
	su -c "git clone https://opendev.org/openstack-dev/devstack -b stable/stein" -s /bin/bash stack
	cp $rootdir/scripts/vagrant/local.conf /opt/stack/devstack/local.conf
else
	echo "Uknonwn devstack branch"
	exit 1
fi

cd /opt/stack/devstack
su -c "./stack.sh" -s /bin/bash stack
source openrc admin admin
openstack volume type create SPDK --public
