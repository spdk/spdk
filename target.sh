#!/bin/bash

set -e
trap 'ec=$?; if [ $ec -ne 0 ]; then echo "exit $? due to '\$previous_command'"; fi' EXIT
trap 'previous_command=$this_command; this_command=$BASH_COMMAND' DEBUG

sudo pkill -f nvmf_tgt

if ! [ -e "lib/nvme/nvme.o" ]; then
  git submodule update --init
  ./configure
  make -j$(getconf _NPROCESSORS_ONLN)
fi

mkdir -p /dev/hugepages
mountpoint -q /dev/hugepages || mount -t hugetlbfs nodev /dev/hugepages
sudo /bin/bash -c "echo 1024 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages"
sudo build/bin/nvmf_tgt -m 0xf

