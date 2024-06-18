#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

function sma_waitforlisten() {
	local sma_addr=${1:-127.0.0.1}
	local sma_port=${2:-8080}

	for ((i = 0; i < 5; i++)); do
		if nc -z $sma_addr $sma_port; then
			return 0
		fi
		sleep 1s
	done
	return 1
}

function uuid2base64() {
	python <<- EOF
		import base64, uuid
		print(base64.b64encode(uuid.UUID("$1").bytes).decode())
	EOF
}

get_cipher() {
	case "$1" in
		AES_CBC) echo 0 ;;
		AES_XTS) echo 1 ;;
		*) echo "$1" ;;
	esac
}

format_key() {
	base64 -w 0 <(echo -n "$1")
}

uuid2nguid() {
	# The NGUID returned by the RPC is UPPERCASE
	local uuid=${1^^}
	echo ${uuid//-/}
}

get_qos_caps() {
	local rootdir

	rootdir="$(dirname $BASH_SOURCE)/../.."

	"$rootdir/scripts/sma-client.py" <<- EOF
		{
		  "method": "GetQosCapabilities",
		  "params": {
		    "device_type": $1
		  }
		}
	EOF
}
