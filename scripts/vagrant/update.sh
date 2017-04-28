#!/bin/bash

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
    yum install -y redhat-lsb
    DISTRIB_ID=`lsb_release -si`
    DISTRIB_RELEASE=`lsb_release -sr`
    DISTRIB_CODENAME=`lsb_release -sc`
    DISTRIB_DESCRIPTION=`lsb_release -sd`
fi

# Do initial setup for the system
if [ $DISTRIB_ID == "Ubuntu" ]; then

    export DEBIAN_PRIORITY=critical
    export DEBIAN_FRONTEND=noninteractive
    export DEBCONF_NONINTERACTIVE_SEEN=true
    APT_OPTS="--assume-yes --no-install-suggests --no-install-recommends -o Dpkg::Options::=\"--force-confdef\" -o Dpkg::Options::=\"--force-confold\""

    # Standard update + upgrade dance
    apt-get update ${APT_OPTS}
    apt-get upgrade ${APT_OPTS}

    # Install useful but non-mandatory tools
    apt-get install -y gdb git
elif [ $DISTRIB_ID == "CentOS" ]; then
    # Standard update + upgrade dance
    yum check-update
    yum update -y
fi
