#!/bin/bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

(
	cat << 'END'
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of __COMPANY__ nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
END
) > /tmp/c.txt

(
	cat << 'END'
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
#    * Neither the name of __COMPANY__ nor the names of its
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
END
) > /tmp/makefile.txt

function get_sha() {
	sha=
	start=$(cat -n $1 | grep "Redistribution and use" | awk '{print $1}')
	end=$(cat -n $1 | grep "POSSIBILITY OF SUCH DAMAGE" | head -1 | awk '{print $1}')
	if [ -z $start ] || [ -z $end ]; then
		return
	fi
	count=$((end - start + 1))
	sha=$(sed -n "${start},+${count}p" $1 | sha1sum | awk '{print $1}')
}

intel_c_sha=$(sed 's/__COMPANY__/Intel Corporation/g' /tmp/c.txt | sha1sum | awk '{print $1}')
nvidia_c_sha=$(sed 's/__COMPANY__/Nvidia Corporation/g' /tmp/c.txt | sha1sum | awk '{print $1}')
samsung_c_sha=$(sed 's/__COMPANY__/Samsung Electronics Co., Ltd./g' /tmp/c.txt | sha1sum | awk '{print $1}')
eideticom_c_sha=$(sed 's/__COMPANY__/Eideticom Inc/g' /tmp/c.txt | sha1sum | awk '{print $1}')
generic_c_sha=$(sed 's/__COMPANY__/the copyright holder/g' /tmp/c.txt | sha1sum | awk '{print $1}')

for f in $(git ls-files '**/*.c' '**/*.cpp' '**/*.h' '**/*.cc' '**/*.go'); do
	get_sha $f
	if [[ $sha == "$intel_c_sha" ]] \
		|| [[ $sha == "$nvidia_c_sha" ]] \
		|| [[ $sha == "$samsung_c_sha" ]] \
		|| [[ $sha == "$eideticom_c_sha" ]] \
		|| [[ $sha == "$generic_c_sha" ]]; then
		echo $f
		sed -i "$((start - 1)),+$((count))d" $f
		sed -i '1,3d' $f
		sed -i '1 i /*   SPDX-License-Identifier: BSD-3-Clause' $f
	fi

done

intel_makefile_sha=$(sed 's/__COMPANY__/Intel Corporation/g' /tmp/makefile.txt | sha1sum | awk '{print $1}')
nvidia_makefile_sha=$(sed 's/__COMPANY__/Nvidia Corporation/g' /tmp/makefile.txt | sha1sum | awk '{print $1}')
samsung_makefile_sha=$(sed 's/__COMPANY__/Samsung Electronics Co., Ltd./g' /tmp/makefile.txt | sha1sum | awk '{print $1}')
eideticom_makefile_sha=$(sed 's/__COMPANY__/Eideticom Inc/g' /tmp/makefile.txt | sha1sum | awk '{print $1}')
generic_makefile_sha=$(sed 's/__COMPANY__/the copyright holder/g' /tmp/makefile.txt | sha1sum | awk '{print $1}')

for f in $(git ls-files CONFIG MAKEFILE '**/*.mk' '**/Makefile'); do
	get_sha $f
	if [[ $sha == "$intel_makefile_sha" ]] \
		|| [[ $sha == "$nvidia_makefile_sha" ]] \
		|| [[ $sha == "$samsung_makefile_sha" ]] \
		|| [[ $sha == "$eideticom_makefile_sha" ]] \
		|| [[ $sha == "$generic_makefile_sha" ]]; then
		echo $f
		sed -i "$((start - 1)),+$((count))d" $f
		sed -i '1,3d' $f
		sed -i "1 i \#  SPDX-License-Identifier: BSD-3-Clause" $f
	fi

done
