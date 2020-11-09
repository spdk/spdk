#!/usr/bin/env bash

install_liburing() {
	local GIT_REPO_LIBURING=https://github.com/axboe/liburing.git
	local liburing_dir=/usr/local/src/liburing

	if [[ $(ldconfig -p) == *liburing.so* ]]; then
		echo "liburing is already installed. skipping"
	else
		if [[ -d $liburing_dir ]]; then
			echo "liburing source already present, not cloning"
		else
			mkdir -p $liburing_dir
			git clone "${GIT_REPO_LIBURING}" "$liburing_dir"
		fi
		# Use commit we know we can compile against. See #1673 as a reference.
		# FIXME: Switch to liburing-2.0 when it's finally released
		git -C "$liburing_dir" checkout 5d027b315d78415a31dcc9111f6bd8924ba5b4e6
		(cd "$liburing_dir" && ./configure --libdir=/usr/lib64 && make install)
	fi
}

install_shfmt() {
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

install_spdk_bash_completion() {
	[[ -e /usr/share/bash-completion/bash_completion ]] || return 0

	local compat_dir=/etc/bash_completion.d
	mkdir -p "$compat_dir"

	if [[ ! -e $compat_dir/spdk ]]; then
		ln -vs "$scriptsdir/bash-completion/spdk" "$compat_dir"
	fi
}

if [[ $INSTALL_DEV_TOOLS == true ]]; then
	install_shfmt
	install_spdk_bash_completion
fi

if [[ $INSTALL_LIBURING == true ]]; then
	install_liburing
fi
