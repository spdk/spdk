#!/usr/bin/env bash

http_proxy=$1

cat <<- PROXY >> /etc/profile
	export http_proxy=${http_proxy}
	export https_proxy=${http_proxy}
PROXY
echo "pkg_env: {http_proxy: ${http_proxy}" > /usr/local/etc/pkg.conf
chown root:wheel /usr/local/etc/pkg.conf
chmod 644 /usr/local/etc/pkg.conf
