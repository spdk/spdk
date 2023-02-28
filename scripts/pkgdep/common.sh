#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
install_liburing() {
	local GIT_REPO_LIBURING=https://github.com/axboe/liburing.git
	local liburing_dir=/usr/local/src/liburing

	if [[ -d $liburing_dir ]]; then
		echo "liburing source already present, not cloning"
	else
		mkdir -p $liburing_dir
		git clone "${GIT_REPO_LIBURING}" "$liburing_dir"
	fi
	# Use commit we know we can compile against. See #1673 as a reference.
	git -C "$liburing_dir" checkout liburing-2.2
	(cd "$liburing_dir" && ./configure --libdir=/usr/lib64 --libdevdir=/usr/lib64 && make install)
	echo /usr/lib64 > /etc/ld.so.conf.d/spdk-liburing.conf
	ldconfig
}

install_shfmt() {
	# Fetch version that has been tested
	local shfmt_version=3.1.0
	local shfmt=shfmt-$shfmt_version
	local shfmt_dir=${SHFMT_DIR:-/opt/shfmt}
	local shfmt_dir_out=${SHFMT_DIR_OUT:-/usr/bin}
	local shfmt_url
	local os
	local arch

	if hash "$shfmt" && [[ $("$shfmt" --version) == "v$shfmt_version" ]]; then
		echo "$shfmt already installed"
		return 0
	fi 2> /dev/null

	arch=$(uname -m)
	os=$(uname -s)

	case "$arch" in
		x86_64) arch="amd64" ;;
		aarch64) arch="arm" ;;
		*)
			echo "Not supported arch (${arch:-Unknown}), skipping"
			return 0
			;;
	esac

	case "$os" in
		Linux) shfmt_url=https://github.com/mvdan/sh/releases/download/v$shfmt_version/shfmt_v${shfmt_version}_linux_${arch} ;;
		FreeBSD) shfmt_url=https://github.com/mvdan/sh/releases/download/v$shfmt_version/shfmt_v${shfmt_version}_freebsd_${arch} ;;
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
		cp -v "$scriptsdir/bash-completion/spdk" "$compat_dir"
	fi
}

install_markdownlint() {
	local git_repo_mdl="https://github.com/markdownlint/markdownlint.git"
	local mdl_version="v0.11.0"
	if [ ! -d /usr/src/markdownlint ]; then
		sudo -E git clone --branch "$mdl_version" "$git_repo_mdl" "/usr/src/markdownlint"
		(
			cd /usr/src/markdownlint
			if ! hash rake &> /dev/null; then
				sudo -E gem install rake
			fi
			if ! hash bundler &> /dev/null; then
				sudo -E gem install bundler
			fi
			sudo -E rake install
		)
	else
		echo "Markdown lint tool already in /usr/src/markdownlint. Not installing"
	fi
}

if [[ $INSTALL_DEV_TOOLS == true ]]; then
	install_shfmt
	install_spdk_bash_completion
	if [[ $ID != centos && $ID != rocky && $ID != sles ]]; then
		install_markdownlint
	else
		echo "mdl not supported on $ID, disabling"
	fi
fi

if [[ $INSTALL_LIBURING == true ]]; then
	install_liburing
fi
