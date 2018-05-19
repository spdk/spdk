#!/usr/bin/env bash

# centos7 also supported
#export SPDK_VAGRANT_DISTRO="centos7"
#export SPDK_VAGRANT_DISTRO="ubuntu16"
#export SPDK_VAGRANT_DISTRO="ubuntu18"
export SPDK_VAGRANT_DISTRO="fedora26"
#export SPDK_VAGRANT_DISTRO="fedora27"
#export SPDK_VAGRANT_DISTRO="freebsd11"

export SPDK_VAGRANT_VMCPU=4
export SPDK_VAGRANT_VMRAM=4096

# the following disable rsync of the spdk directory
#export SPDK_DIR="none"

# replace this with the absolute path of your spdk repository directory
export SPDK_DIR="../../"
