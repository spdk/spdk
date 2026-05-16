#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2025 Nutanix Inc
#  All rights reserved.
#

function get_color() {
	local requested_color=${1:-green}
	local -A color_pallete=()

	local foreground="\e[38;5"
	local background="\e[48;5"

	color_pallete['none']=""
	color_pallete['reset']='\e[0m'
	color_pallete["underline"]='\e[4m'
	color_pallete["red"]='\e[31m'
	color_pallete["green"]='\e[32m'
	color_pallete["yellow"]='\e[33m'
	color_pallete['blue']='\e[34m'
	color_pallete['magenta']='\e[35m'
	color_pallete['cyan']='\e[36m'
	color_pallete['whilte']='\e[37m'
	color_pallete["orange"]="$foreground;208m"

	[[ -n ${color_pallete["$requested_color"]} ]] || return 1

	printf '%b' "${color_pallete["$requested_color"]}"
}

function colorize() {
	xtrace_disable

	local color=${1:-none} str=$2

	if [[ $AUTOTEST_COLORS_ENABLED == yes && $color != none ]]; then
		printf '%s\n' "$(get_color "$color")${str}$(get_color reset)"
	else
		# Just pass as-is
		printf '%s\n' "$str"
	fi

	xtrace_restore
}

function underline() {
	colorize underline "$*"
}

export AUTOTEST_COLORS_ENABLED=${AUTOTEST_COLORS_ENABLED:-yes}
