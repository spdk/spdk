#! /usr/bin/env bash

#
#  BSD LICENSE
#
#  Copyright (c) 2018 by NetApp, Inc.
#  All Rights Reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

if [ -z "${MAKE}" ]; then
	export MAKE=make
fi

if [ -z "${GIT}" ]; then
	export GIT=git
fi

if [ -z "${GREP}" ]; then
	export GREP=grep
fi

if [ -z "${READLINK}" ]; then
	export READLINK=readlink
fi

AUTOTEST_DRIVER_PATH=$($READLINK -f ${BASH_SOURCE%/*})
TIMESTAMP=`date +"%Y%m%d%H%M%S"`
BUILD_NAME="build-${TIMESTAMP}"
SPDK_AUTOTEST_LOCAL_PATH=$PWD
SPDK_SOURCE_PATH=$($READLINK -f $SPDK_AUTOTEST_LOCAL_PATH)

# The command line help
display_help() {
	echo
	echo "Usage: $0 [-h] | [-q]"
	echo "  -q : No output to screen"
	echo "  -n : No-op: dry-run w/out calling autorun.sh"
	echo "  -h : This help"
	echo
	echo "  Must be called from inside an spdk repository. Rsyncs the repository to"
	echo "  a directory named ${PWD}/../<branch>/${BUILD_NAME}"
	echo "  and runs autorun.sh."
	echo
	echo "Example:"
	echo "    run-spdk-autotest.sh -q"
	echo "    run-spdk-autotest.sh -n"
	echo
}

set -e

NOOP=0
METHOD=1
V=1
OPTIND=1         # Reset in case getopts has been used previously in the shell.
while getopts "qhn" opt; do
	case "$opt" in
		q)  V=0
		;;
		n)  NOOP=1
		;;
		h)  display_help >&2
			exit 0
		;;
	esac
done

# The following code verifies the input parameters and sets up the following variables:
#
# SPDK_AUTOTEST_LOCAL_PATH
# GIT_REPO_PATH
# GIT_BRANCH
#

case "$METHOD" in
	1)
		if [ ! -d "${SPDK_SOURCE_PATH}" ]; then
			echo "${SPDK_SOURCE_PATH} does not exist!"
			exit 1
		fi
                if [ ! -d "${SPDK_SOURCE_PATH}/.git" ]; then
                        echo "${SPDK_SOURCE_PATH} is not a git repository"
                        exit 1
                fi

		GIT_REPO_SRC_DIR=$($READLINK -f "${SPDK_SOURCE_PATH}" | tr -t '/' ' ' | awk '{print $NF}')

		if [ ! "${GIT_REPO_SRC_DIR}" = "spdk" ]; then
			echo "The ${SPDK_SOURCE_PATH} git repository is not named \"spdk\""
			exit 1
		fi

                GIT_REPO_SRC=$(git rev-parse --show-toplevel)
		GIT_BRANCH=$(git rev-parse --abbrev-ref HEAD)

		if [ "${SPDK_AUTOTEST_LOCAL_PATH}" = "${SPDK_SOURCE_PATH}" ]; then
			SPDK_AUTOTEST_LOCAL_PATH=$($READLINK -f ${SPDK_AUTOTEST_LOCAL_PATH}/..)
                        echo "Set SPDK_AUTOTEST_LOCAL_PATH to ${SPDK_AUTOTEST_LOCAL_PATH}"
		fi

		if [ -d "${SPDK_AUTOTEST_LOCAL_PATH}/${GIT_BRANCH}" ]; then
			if [ -d "${SPDK_AUTOTEST_LOCAL_PATH}/${GIT_BRANCH}/.git" ]; then
				echo "${SPDK_AUTOTEST_LOCAL_PATH}/${GIT_BRANCH} is a git repository!"
				exit 1
			fi
		fi

		GIT_REPO_PATH="${SPDK_AUTOTEST_LOCAL_PATH}/${GIT_BRANCH}/${BUILD_NAME}"
	;;
	*)
		echo "Internal Error: Must specify a source path or branch name"
		display_help
		exit 1
	;;
esac

AUTOTEST_RESULTS="${GIT_REPO_PATH}"
AUTOTEST_OUTPUT_PATH="${GIT_REPO_PATH}/output"
rootdir="${GIT_REPO_PATH}/spdk"
BUILD_LOG_FILE="${AUTOTEST_OUTPUT_PATH}/build.log"

if [[ ${NOOP} -eq 1 ]]; then
	echo "AUTOTEST_DRIVER_PATH $AUTOTEST_DRIVER_PATH"
	#echo "SPDK_AUTOTEST_LOCAL_PATH $SPDK_AUTOTEST_LOCAL_PATH"
	echo "AUTOTEST_OUTPUT_PATH $AUTOTEST_OUTPUT_PATH"
	#echo "rootdir $rootdir"
	echo "BUILD_LOG_FILE $BUILD_LOG_FILE"
	#echo "GIT_BRANCH $GIT_BRANCH"
	#echo "BUILD_NAME $BUILD_NAME"
	echo "GIT_REPO_PATH $GIT_REPO_PATH"
	echo "AUTOTEST_RESULTS $AUTOTEST_RESULTS"
fi

if [[ -e ~/autorun-spdk.conf ]]; then
	conf=~/autorun-spdk.conf
fi

if [[ -z $conf ]]; then
	echo "~/autorun-spdk.conf file not found."
	exit 1
fi

if [[ ${NOOP} -eq 0 ]]; then
	mkdir -pv --mode=775 "${AUTOTEST_OUTPUT_PATH}"
	rm -f latest
	echo V=$V
	if [[ $V -eq 0 ]]; then
		echo Quieting output
		exec 3>&1 4>&2 > "${BUILD_LOG_FILE}" 2>&1
	else
		echo Teeing to ${BUILD_LOG_FILE}
		exec > >(tee -a "${BUILD_LOG_FILE}") 2>&1
	fi

	echo Using conf file at $conf
	cat $conf
	echo

	case "$METHOD" in
		1)
			echo "rsync git repository from ${GIT_REPO_SRC} to ${GIT_REPO_PATH}"
			rsync -av "${GIT_REPO_SRC}" "${GIT_REPO_PATH}"
			pushd "${GIT_REPO_PATH}/spdk"
			sudo "${MAKE}" clean -j $(nproc)
			sudo "${GIT}" clean -d -f
			popd
		;;
		*)
			echo "Internal Error: Must specify a source path or branch name"
			display_help
			exit 1
		;;
	esac

	ln -sv ${GIT_REPO_PATH} latest
	pushd "${AUTOTEST_OUTPUT_PATH}"
	export output_dir="${AUTOTEST_OUTPUT_PATH}"

	# Runs agent scripts
	"${rootdir}/autobuild.sh" "$conf"
	sudo -E "${rootdir}/autotest.sh" "$conf"
	"${rootdir}/autopackage.sh" "$conf"
	sudo -E "${rootdir}/autorun_post.py" -d "${AUTOTEST_OUTPUT_PATH}" -r "${rootdir}"

	echo "All Tests Passed" > ${GIT_REPO_PATH}/passed

	# Redirect back to screen
	if [[ $V -eq 0 ]]; then
		echo Redirect to screen
		exec 1>&3 2>&4 > >(tee -a "${BUILD_LOG_FILE}") 2>&1
	fi
	popd

	echo "All tests passed"
fi

echo Output directory: ${GIT_REPO_PATH}
echo Build log: "${BUILD_LOG_FILE}"
