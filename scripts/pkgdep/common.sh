#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
install_liburing() {
	local GIT_REPO_LIBURING=https://github.com/axboe/liburing.git
	local liburing_dir=/usr/local/src/liburing
	local liburing_version="liburing-2.2"

	rm -rf "$liburing_dir"
	# Use commit we know we can compile against. See #1673 as a reference.
	git clone --branch "${liburing_version}" "${GIT_REPO_LIBURING}" "${liburing_dir}"

	(cd "$liburing_dir" && ./configure --libdir=/usr/lib64 --libdevdir=/usr/lib64 && make install)
	echo /usr/lib64 > /etc/ld.so.conf.d/spdk-liburing.conf
	ldconfig -X
}

install_uadk() {
	local GIT_REPO_UADK=https://github.com/Linaro/uadk
	local uadk_dir=/usr/local/src/uadk
	local uadk_version="v2.9.1"

	rm -rf "$uadk_dir"
	git clone --branch "${uadk_version}" "${GIT_REPO_UADK}" "${uadk_dir}"

	(cd "$uadk_dir" && ./cleanup.sh && ./autogen.sh && ./configure --libdir=/usr/lib64 && make install)
	echo /usr/lib64 > /etc/ld.so.conf.d/uadk.conf
	ldconfig
}

_install_shfmt() {
	local version=$1 arch=$2 type=$3

	local shfmt_dir=/usr/local/src/shfmt shfmt_repo=https://github.com/mvdan/sh

	if [[ $type != build ]]; then
		echo "$shfmt_repo/releases/download/$version/shfmt_${version}_linux_${arch}"
		return 0
	fi

	if [[ ! -e /etc/opt/spdk-pkgdep/paths/go.path ]]; then
		install_golang
	fi
	source /etc/opt/spdk-pkgdep/paths/export.sh > /dev/null

	rm -rf "$shfmt_dir"
	git clone --branch "$version" "$shfmt_repo" "$shfmt_dir"

	# See cmd/shfmt/Dockerfile
	CGO_ENABLED=0 go -C "$shfmt_dir" build \
		-ldflags "-w -s -extldflags '-static' -X main.version=$version" \
		"$shfmt_dir/cmd/shfmt"

	echo "file://$shfmt_dir/shfmt"
}

install_shfmt() {
	# Fetch version that has been tested
	local shfmt_version=v3.8.0
	local shfmt=shfmt-$shfmt_version
	local shfmt_dir=${SHFMT_DIR:-/opt/shfmt}
	local shfmt_dir_out=${SHFMT_DIR_OUT:-/usr/bin}
	local shfmt_url
	local os
	local arch
	local cmdline

	if hash "$shfmt" && [[ $("$shfmt" --version) == "$shfmt_version" ]]; then
		echo "$shfmt already installed"
		return 0
	fi 2> /dev/null

	arch=$(uname -m)
	os=$(uname -s)

	case "$arch" in
		x86_64) arch="amd64" ;;
		aarch) arch="arm" ;;
		aarch64) arch="arm64" ;;
		amd64) ;; # FreeBSD
		*)
			echo "Not supported arch (${arch:-Unknown}), skipping"
			return 0
			;;
	esac

	cmdline=("$shfmt_version" "$arch")
	[[ $os == FreeBSD || -n $BUILD_SHFMT ]] && cmdline+=(build)

	shfmt_url=$(_install_shfmt "${cmdline[@]}")

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
	local srcdir=/usr/src/markdownlint

	rm -rf "$srcdir"
	git clone --branch "$mdl_version" "$git_repo_mdl" "$srcdir"
	(
		cd "$srcdir"
		rake install
	)
}

install_protoc() {
	local PROTOCVERSION=${PROTOCVERSION:-21.7}
	local PROTOCGENGOVERSION=${PROTOCGENGOVERSION:-1.28}
	local PROTOCGENGOGRPCVERSION=${PROTOCGENGOGRPCVERSION:-1.2}
	local protocdir protocpkg protocurl protocver arch
	local skip_protoc=0
	# It is easy to find an incompatible combination of protoc,
	# protoc-gen-go and protoc-gen-go-grpc. Therefore install a
	# preferred version combination even if more recent versions
	# of some of these tools would be available in the system.
	protocver=$(protoc --version 2> /dev/null | {
		read -r _ v
		echo $v
	})
	if [[ "3.${PROTOCVERSION}" == "${protocver}" ]]; then
		echo "found protoc version ${protocver} exactly required 3.${PROTOCVERSION}, skip installing"
		skip_protoc=1
	fi
	protocdir=/opt/protoc/${PROTOCVERSION}
	if [[ -x "${protocdir}/bin/protoc" ]]; then
		echo "protoc already installed to ${protocdir}, skip installing"
		skip_protoc=1
	fi
	if [[ "${skip_protoc}" != "1" ]]; then
		echo "installing protoc v${PROTOCVERSION} to ${protocdir}"
		mkdir -p "${protocdir}"
		arch=x86_64
		if [[ "$(uname -m)" == "aarch64" ]]; then
			arch=aarch_64
		fi
		protocpkg=protoc-${PROTOCVERSION}-linux-${arch}.zip
		protocurl=https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOCVERSION}/${protocpkg}
		curl -f -LO "${protocurl}" || {
			echo "downloading protoc ${PROTOCVERSION} from ${protocurl} failed"
			return 1
		}
		unzip -d "${protocdir}" "${protocpkg}" || {
			echo "extracting protoc ${PROTOCVERSION} from ${protocpkg} failed"
			rm -f "${protocpkg}"
			return 1
		}
		rm -f "${protocpkg}"
	fi
	if [[ -x "${protocdir}/bin/protoc-gen-go" ]]; then
		echo "${protocdir}/bin/protoc-gen-go already installed"
	else
		echo "installing protoc-gen-go v${PROTOCGENGOVERSION} to ${protocdir}/bin"
		mkdir -p "${protocdir}/bin"
		GOBIN="${protocdir}/bin" go install "google.golang.org/protobuf/cmd/protoc-gen-go@v${PROTOCGENGOVERSION}" || {
			echo "protoc protoc-gen-go plugin install failed"
			return 1
		}
	fi
	if [[ -x "${protocdir}/bin/protoc-gen-go-grpc" ]]; then
		echo "${protocdir}/bin/protoc-gen-go-grpc already installed"
	else
		echo "installing protoc-gen-go-grpc v${PROTOCGENGOGRPCVERSION} to ${protocdir}/bin"
		mkdir -p "${protocdir}/bin"
		GOBIN="${protocdir}/bin" go install "google.golang.org/grpc/cmd/protoc-gen-go-grpc@v${PROTOCGENGOGRPCVERSION}" || {
			echo "protoc protoc-gen-go-grpc plugin install failed"
			return 1
		}
	fi
	pkgdep_toolpath protoc "${protocdir}/bin"
}

install_golang() {
	local GOVERSION=${GOVERSION:-1.21.1}
	local godir gopkg gover arch os

	read -r _ _ gover _ < <(go version) || true
	gover=${gover#go}
	if [[ -n "${gover}" ]] && ge "${gover}" "${GOVERSION}"; then
		echo "found go version ${gover} >= required ${GOVERSION}, skip installing"
		return 0
	fi
	godir=/opt/go/${GOVERSION}
	if [[ -x "${godir}/bin/go" ]]; then
		echo "go already installed in ${godir}, skip installing"
		return 0
	fi
	mkdir -p "${godir}"
	arch=amd64
	os=$(uname -s)
	if [[ "$(uname -m)" == "aarch64" ]]; then
		arch=arm64
	fi
	gopkg=go${GOVERSION}.${os,,}-${arch}.tar.gz
	echo "installing go v${GOVERSION} to ${godir}/bin"
	curl -sL https://go.dev/dl/${gopkg} | tar -C "${godir}" -xzf - --strip 1
	if ! "${godir}/bin/go" version; then
		echo "go install failed"
		return 1
	fi
	export PATH=${godir}/bin:$PATH
	export GOBIN=${godir}/bin
	pkgdep_toolpath go "${godir}/bin"
}

install_golangci_lint() {
	local golangcidir installed_lintversion lintversion=${GOLANGCLILINTVERSION:-1.54.2}
	installed_lintversion=$(golangci-lint --version | awk '{print $4}')

	if [[ -n "${installed_lintversion}" ]] && ge "${installed_lintversion}" "${lintversion}"; then
		echo "golangci-lint already installed, skip installing"
		return 0
	fi

	echo "installing golangci-lint"
	golangcidir=/opt/golangci/$lintversion/bin
	export PATH=${golangcidir}:$PATH
	curl -sSfL https://raw.githubusercontent.com/golangci/golangci-lint/v${lintversion}/install.sh \
		| sh -s -- -b "${golangcidir}" "v$lintversion" || {
		echo "installing golangci-lint failed"
		return 1
	}
	pkgdep_toolpath golangci_lint "${golangcidir}"
}

if [[ $INSTALL_DEV_TOOLS == true ]]; then
	install_shfmt
	install_spdk_bash_completion
	if [[ $ID != centos && $ID != rocky && $ID != sles ]]; then
		# The gem mdl 0.11.0 depends on mixlib-shellout, which requires Ruby ≥ 3.0. While 20.04.6 LTS (Focal Fossa) has Ruby 2.7.0.
		[[ $ID == ubuntu && $VERSION_CODENAME == focal ]] && gem install mixlib-shellout -v 3.3.8
		install_markdownlint
	else
		echo "mdl not supported on $ID, disabling"
	fi
fi

if [[ $INSTALL_LIBURING == true ]]; then
	install_liburing
fi

if [[ $INSTALL_UADK == true ]]; then
	install_uadk
fi

if [[ $INSTALL_GOLANG == true ]]; then
	install_golang
	[[ $(uname -s) == Linux ]] && install_protoc
	install_golangci_lint
fi
