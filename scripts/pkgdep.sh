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
	echo "  -p --pmem                   Additional dependencies for reduce and pmdk"
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

function install_liburing() {
	local GIT_REPO_LIBURING=https://github.com/axboe/liburing.git
	local liburing_dir=/usr/local/src/liburing

	if [[ -e /usr/lib64/liburing.so ]]; then
		echo "liburing is already installed. skipping"
	else
		if [[ -d $liburing_dir ]]; then
			echo "liburing source already present, not cloning"
		else
			mkdir $liburing_dir
			git clone "${GIT_REPO_LIBURING}" "$liburing_dir"
		fi
		(cd "$liburing_dir" && ./configure --libdir=/usr/lib64 && make install)
	fi
}

function install_shfmt() {
	# Fetch version that has been tested
	local shfmt_version=3.1.0
	local shfmt=shfmt-$shfmt_version
	local shfmt_dir=${SHFMT_DIR:-/opt/shfmt}
	local shfmt_dir_out=${SHFMT_DIR_OUT:-/usr/bin}
	local shfmt_url
	local os

	if hash "$shfmt" && [[ $("$shfmt" --version) == "v$shfmt_version" ]]; then
		echo "$shfmt already installed"
		return 0
	fi 2> /dev/null

	os=$(uname -s)

	case "$os" in
		Linux) shfmt_url=https://github.com/mvdan/sh/releases/download/v$shfmt_version/shfmt_v${shfmt_version}_linux_amd64 ;;
		FreeBSD) shfmt_url=https://github.com/mvdan/sh/releases/download/v$shfmt_version/shfmt_v${shfmt_version}_freebsd_amd64 ;;
		*)
			echo "Not supported OS (${os:-Unknown}), skipping"
			return 0
			;;
	esac

	mkdir -p "$shfmt_dir"
	mkdir -p "$shfmt_dir_out"

	echo "Fetching ${shfmt_url##*/}"...
	local err
	if err=$(curl -f -Lo"$shfmt_dir/$shfmt" "$shfmt_url" 2>&1); then
		chmod +x "$shfmt_dir/$shfmt"
		ln -sf "$shfmt_dir/$shfmt" "$shfmt_dir_out"
	else
		cat <<- CURL_ERR

			* Fetching $shfmt_url failed, $shfmt will not be available for format check.
			* Error:

			$err

		CURL_ERR
		return 0
	fi
	echo "$shfmt installed"
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
fi

ID=${ID:-$OS} ID=${ID,,}

#Link suse related OS to sles
if [[ ${ID,,} == *"suse"* ]]; then
	ID="sles"
fi

if [[ -e $scriptsdir/pkgdep/$ID.sh ]]; then
	source "$scriptsdir/pkgdep/$ID.sh"
else
	printf 'Not supported platform detected (%s), aborting\n' "$ID" >&2
fi
