#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))/../..
source "$rootdir/scripts/common.sh"

# base_bdev will use QAT if available, otherwise AESNI
# base_bdev2 will always use AESNI
# This makes sure that AESNI always gets tested, even if QAT is available.
base_bdev=$1
base_bdev2=$2
if [ -n "$base_bdev" ]; then
	echo '{'
       	echo '  "params": {'
	if [ $(lspci -d:37c8 | wc -l) -eq 0 ]; then
		echo '    "crypto_pmd": "crypto_aesni_mb",'
	else
		echo '    "crypto_pmd": "crypto_qat",'
	fi
        echo '    "name": "crypto_ram",'
        echo '    "base_bdev_name": "'$base_bdev'",'
        echo '    "key": "9012345678912345"'
	echo '  },'
       	echo '  "method": "bdev_crypto_create"'
fi
if [ -n "$base_bdev2" ]; then
	echo '},'
	echo '{'
       	echo '  "params": {'
        echo '    "crypto_pmd": "crypto_aesni_mb",'
       	echo '    "name": "crypto_ram2",'
        echo '    "base_bdev_name": "'$base_bdev2'",'
       	echo '    "key": "9012345678912345"'
        echo '  },'
       	echo '  "method": "bdev_crypto_create"'
	echo '}'
else
	echo '}'
fi
