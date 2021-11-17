#!/usr/bin/env bash
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
	echo "  -p --pmem                   Additional dependencies for reduce, pmdk and pmdkobj"
	echo "  -f --fuse                   Additional dependencies for FUSE and NVMe-CUSE"
	echo "  -r --rdma                   Additional dependencies for RDMA transport in NVMe over Fabrics"
	echo "  -b --docs                   Additional dependencies for building docs"
	echo "  -u --uring                  Additional dependencies for io_uring"
	echo ""
	exit 0
}

function install_all_dependencies() {
	INSTALL_DEV_TOOLS=true
	INSTALL_PMEM=true
	INSTALL_FUSE=true
	INSTALL_RDMA=true
	INSTALL_DOCS=true
	INSTALL_LIBURING=true
}

INSTALL_CRYPTO=false
INSTALL_DEV_TOOLS=false
INSTALL_PMEM=false
INSTALL_FUSE=false
INSTALL_RDMA=false
INSTALL_DOCS=false
INSTALL_LIBURING=false

while getopts 'abdfhipru-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage ;;
				all) install_all_dependencies ;;
				developer-tools) INSTALL_DEV_TOOLS=true ;;
				pmem) INSTALL_PMEM=true ;;
				fuse) INSTALL_FUSE=true ;;
				rdma) INSTALL_RDMA=true ;;
				docs) INSTALL_DOCS=true ;;
				uring) INSTALL_LIBURING=true ;;
				*)
					echo "Invalid argument '$OPTARG'"
					usage
					;;
			esac
			;;
		h) usage ;;
		a) install_all_dependencies ;;
		d) INSTALL_DEV_TOOLS=true ;;
		p) INSTALL_PMEM=true ;;
		f) INSTALL_FUSE=true ;;
		r) INSTALL_RDMA=true ;;
		b) INSTALL_DOCS=true ;;
		u) INSTALL_LIBURING=true ;;
		*)
			echo "Invalid argument '$OPTARG'"
			usage
			;;
	esac
done

trap 'set +e; trap - ERR; echo "Error!"; exit 1;' ERR

scriptsdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $scriptsdir/..)

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

for id in $ID $ID_LIKE; do
	if [[ -e $scriptsdir/pkgdep/$id.sh ]]; then
		source "$scriptsdir/pkgdep/$id.sh"
		source "$scriptsdir/pkgdep/common.sh"
		exit 0
	fi
done

printf 'Not supported distribution detected (%s), aborting\n' "$ID" >&2
exit 1
