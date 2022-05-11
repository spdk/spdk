#!/usr/bin/env bash

# Install main dependencies
pacman -Sy --needed --noconfirm gcc make cunit libaio openssl \
	libutil-linux libiscsi python ncurses json-c cmocka ninja meson
# Additional dependencies for SPDK CLI
pacman -Sy --needed --noconfirm python-pexpect python-pip libffi
pip install configshell_fb
pip install pyelftools
pip install ijson
pip install python-magic
# Additional dependencies for DPDK
pacman -Sy --needed --noconfirm numactl nasm
# Additional dependencies for ISA-L used in compression
pacman -Sy --needed --noconfirm autoconf automake libtool help2man
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	pacman -Sy --needed --noconfirm git astyle autopep8 \
		clang sg3_utils pciutils shellcheck bash-completion
	#fakeroot needed to instal via makepkg
	pacman -Sy --needed --noconfirm fakeroot
	su - $SUDO_USER -c "pushd /tmp;
		git clone https://aur.archlinux.org/perl-perlio-gzip.git;
		cd perl-perlio-gzip;
		yes y | makepkg -si --needed;
		cd ..; rm -rf perl-perlio-gzip
		popd"
	# sed is to modify sources section in PKGBUILD
	# By default it uses git:// which will fail behind proxy, so
	# redirect it to http:// source instead
	su - $SUDO_USER -c "pushd /tmp;
		git clone https://aur.archlinux.org/lcov-git.git;
		cd lcov-git;
		sed -i 's/git:/git+http:/' PKGBUILD;
		makepkg -si --needed --noconfirm;
		cd .. && rm -rf lcov-git;
		popd"
fi
if [[ $INSTALL_PMEM == "true" ]]; then
	# Additional dependencies for building pmem based backends
	pacman -Sy --needed --noconfirm ndctl pkg-config
	git clone https://github.com/pmem/pmdk.git /tmp/pmdk -b 1.6.1
	make -C /tmp/pmdk -j$(nproc)
	make install prefix=/usr -C /tmp/pmdk
	echo "/usr/local/lib" > /etc/ld.so.conf.d/pmdk.conf
	ldconfig
	rm -rf /tmp/pmdk
fi
if [[ $INSTALL_FUSE == "true" ]]; then
	# Additional dependencies for FUSE and NVMe-CUSE
	pacman -Sy --needed --noconfirm fuse3
fi
if [[ $INSTALL_RDMA == "true" ]]; then
	# Additional dependencies for RDMA transport in NVMe over Fabrics
	if [[ -n "$http_proxy" ]]; then
		gpg_options=" --keyserver hkp://pgp.mit.edu:11371 --keyserver-options \"http-proxy=$http_proxy\""
	fi
	su - $SUDO_USER -c "gpg $gpg_options --recv-keys 29F0D86B9C1019B1"
	su - $SUDO_USER -c "pushd /tmp;
		git clone https://aur.archlinux.org/rdma-core.git;
		cd rdma-core;
		makepkg -si --needed --noconfirm;
		cd .. && rm -rf rdma-core;
		popd"
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	pacman -Sy --needed --noconfirm doxygen graphviz
	pacman -S --noconfirm --needed gd ttf-font
	su - $SUDO_USER -c "pushd /tmp;
		git clone https://aur.archlinux.org/mscgen.git;
		cd mscgen;
		makepkg -si --needed --noconfirm;
		cd .. && rm -rf mscgen;
		popd"
fi
