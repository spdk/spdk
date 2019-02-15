#!/bin/bash

SPDK_PATH=/home/vagrant/spdk_repo/spdk
SDL_DIR=$SPDK_PATH/../sdl_scans

export PROTEX_PATH=/usr/local/protex/protex/bin

PATH=${PROTEX_PATH}:${PATH}
export PATH

function get_spdk() {
        branch="$1"

        cd $SPDK_PATH

        git clean -xdffq
        git submodule foreach --recursive git clean -xdffq

	git fetch "https://review.gerrithub.io/spdk/spdk" --recurse-submodules=yes $branch
        git reset --hard 
	git checkout FETCH_HEAD
}

function scan_protex() {
        # Getting SPDK repo
        branch="$1"
        get_spdk $branch
        cd $SPDK_PATH

	# Prepare list of directories to scan
	find ./ -maxdepth 1 -type d -printf "%f\n" | grep -v ^\\. > dirs.txt
	git config --file .gitmodules --get-regexp path | awk '{ print $2 }' > submodules.txt
	sort dirs.txt submodules.txt | uniq -u > $SDL_DIR/protex.txt
	sed 's/$/\//' $SDL_DIR/protex.txt -i
	sed 's/^/\//' $SDL_DIR/protex.txt -i
	rm dirs.txt -f
	rm submodules.txt -f

        # Protex scan
        perl $SDL_DIR/bdscan.pl -prefix $SPDK_PATH -cfg $SDL_DIR/protex.cfg -email
}

scan_protex master
