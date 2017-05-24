#!/usr/bin/env bash
dir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $dir/..)

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

if [ $(uname -s) = Linux ]; then
	# Split each NVMe device into Gpt format.
	if [ "$1" ] && [ $1 =  "Gpt" ]; then

		# skip the output
		$rootdir/scripts/setup.sh reset >> /dev/null
		sleep 5

		echo ""
		echo "[Gpt]"
		for ((j=0; j<$i; j++ ))
		do
			echo " Split Nvme"$j"n1"
			parted -s /dev/nvme"$j"n1 mklabel gpt
			parted -s /dev/nvme"$j"n1 mkpart primary '0%' '50%'
			parted -s /dev/nvme"$j"n1 mkpart primary '50%' '100%'
		done

		# skip the output
		$rootdir/scripts/setup.sh >> /dev/null
	fi
fi
