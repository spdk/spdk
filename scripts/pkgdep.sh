#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#
# Please run this script as root.

set -e

function usage() {
	echo ""
	echo "This script is intended to automate the installation of package dependencies to build SPDK."
	echo "Please run this script as root user or with sudo -E."
	echo ""
	echo "$0"
	echo "  -h --help"
	echo "  -a --all"
	echo "  -d --developer-tools        Install tools for developers (code styling, code coverage, etc.)"
	echo "  -R --rbd                    Additional dependencies for RBD"
	echo "  -r --rdma                   Additional dependencies for RDMA transport in NVMe over Fabrics"
	echo "  -b --docs                   Additional dependencies for building docs"
	echo "  -u --uring                  Additional dependencies for io_uring"
	echo "     --uadk                   Additional dependencies for UADK"
	echo "  -D --daos                   Additional dependencies for DAOS"
	echo "  -A --avahi                  Additional dependencies for Avahi mDNS Discovery"
	echo "  -G --golang                 Additional dependencies for go API generation"
	echo "  -I --idxd                   Additional dependencies for IDXD"
	echo "  -l --lz4                    Additional dependencies for lz4"
	echo ""
	exit 0
}

function install_all_dependencies() {
	INSTALL_DEV_TOOLS=true
	INSTALL_RBD=true
	INSTALL_RDMA=true
	INSTALL_DOCS=true
	INSTALL_LIBURING=true
	INSTALL_DAOS=true
	INSTALL_AVAHI=true
	INSTALL_GOLANG=true
	INSTALL_IDXD=true
	INSTALL_LZ4=true
}

INSTALL_CRYPTO=false
INSTALL_DEV_TOOLS=false
INSTALL_RBD=false
INSTALL_RDMA=false
INSTALL_DOCS=false
INSTALL_LIBURING=false
INSTALL_DAOS=false
INSTALL_AVAHI=false
INSTALL_GOLANG=false
INSTALL_IDXD=false
INSTALL_UADK=false
INSTALL_LZ4=false

while getopts 'abdfhilpruADGIR-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage ;;
				all) install_all_dependencies ;;
				developer-tools) INSTALL_DEV_TOOLS=true ;;
				rbd) INSTALL_RBD=true ;;
				rdma) INSTALL_RDMA=true ;;
				docs) INSTALL_DOCS=true ;;
				uring) INSTALL_LIBURING=true ;;
				uadk) INSTALL_UADK=true ;;
				daos) INSTALL_DAOS=true ;;
				avahi) INSTALL_AVAHI=true ;;
				golang) INSTALL_GOLANG=true ;;
				idxd) INSTALL_IDXD=true ;;
				lz4) INSTALL_LZ4=true ;;
				*)
					echo "Invalid argument '$OPTARG'"
					usage
					;;
			esac
			;;
		h) usage ;;
		a) install_all_dependencies ;;
		d) INSTALL_DEV_TOOLS=true ;;
		R) INSTALL_RBD=true ;;
		r) INSTALL_RDMA=true ;;
		b) INSTALL_DOCS=true ;;
		u) INSTALL_LIBURING=true ;;
		D) INSTALL_DAOS=true ;;
		A) INSTALL_AVAHI=true ;;
		G) INSTALL_GOLANG=true ;;
		I) INSTALL_IDXD=true ;;
		l) INSTALL_LZ4=true ;;
		*)
			echo "Invalid argument '$OPTARG'"
			usage
			;;
	esac
done

trap 'set +e; trap - ERR; echo "Error!"; exit 1;' ERR

scriptsdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $scriptsdir/..)
source "$rootdir/scripts/common.sh"
source "$scriptsdir/pkgdep/helpers.sh"

OS=$(uname -s)

if [[ -e /etc/os-release ]]; then
	source /etc/os-release
elif [[ $OS == FreeBSD ]]; then
	ID=freebsd
else
	ID=unknown
fi

ID=${ID,,}

#Link suse related OS to sles
if [[ $ID == *"suse"* ]]; then
	ID="sles"
fi

# Some distros don't provide these paths in their default $PATH setups, nor
# sudo's secure_path, so add it here. These are needed since gem is most likely
# to put target bins at these locations.
export PATH=$PATH:/usr/local/bin:/usr/local/sbin

for id in $ID $ID_LIKE; do
	if [[ -e $scriptsdir/pkgdep/$id.sh ]]; then
		source "$scriptsdir/pkgdep/$id.sh"
		source "$scriptsdir/pkgdep/common.sh"
		exit 0
	fi
done

printf 'Not supported distribution detected (%s), aborting\n' "$ID" >&2
exit 1
