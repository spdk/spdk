#!/usr/bin/env bash

set -e

case `uname` in
        FreeBSD)
                bdfs=$(pciconf -l | grep "class=0x010802" | awk -F: ' {printf "0000:%02X:%02X.%X\n", $2, $3, $4}')
                ;;
        Linux)
                bdfs=$(lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}')
                ;;
        *)
                exit 1
                ;;
esac

echo "[Nvme]"
i=0
for bdf in $bdfs; do
        echo "  TransportID \"trtype:PCIe traddr:$bdf\" Nvme$i"
        let i=i+1
done
