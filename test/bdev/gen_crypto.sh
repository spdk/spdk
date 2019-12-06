#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/../..
source "$rootdir/scripts/common.sh"

# base_bdev will use QAT if available, otherwise AESNI
# base_bdev2 will always use AESNI
# This makes sure that AESNI always gets tested, even if QAT is available.
base_bdev=$1
base_bdev2=$2

echo
echo "[crypto]"

if [ -n "$base_bdev" ]; then
	if [ $(lspci -d:37c8 | wc -l) -eq 0 ]; then
		echo "  CRY $base_bdev crypto_ram 0123456789123456 crypto_aesni_mb"
	else
		echo "  CRY $base_bdev crypto_ram 0123456789123456 crypto_qat"
	fi
fi

if [ -n "$base_bdev2" ]; then
	echo "  CRY $base_bdev2 crypto_ram2 9012345678912345 crypto_aesni_mb"
fi
