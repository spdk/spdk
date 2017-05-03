Introduction
============

This is a vagrant environment for SPDK with support
for Ubuntu 16.04 and Centos 7.2.

The VM builds SPDK and DPDK from source which can be located at /spdk and /dpdk.

VM Configuration
================

This vagrant environment creates a VM based on environment variables found in ./env.sh
To use, edit env.sh then

    source ./env.sh
    vagrant up

By default, the VM created is/has:
- Ubuntu 16.04
- 2 vCPUs
- 4G of RAM
- 2 NICs (1 x NAT - host access, 1 x private network)

Providers
=========

Currently only the Virtualbox provider is supported.
