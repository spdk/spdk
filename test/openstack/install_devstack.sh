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

if [[ $branch == "master" ]]; then
        cp $rootdir/scripts/vagrant/local_master.conf /opt/stack/devstack/local.conf
elif [[ $branch == "stein" ]]; then
        cp $rootdir/scripts/vagrant/local.conf /opt/stack/devstack/local.conf
else
        echo "Uknonwn devstack branch"
        exit 1
fi

su -c "./stack.sh" -s /bin/bash stack

