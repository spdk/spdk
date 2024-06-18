#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

curdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$curdir/../../")
set -e

source "$curdir/irqs.sh"

vm_interrupts=("$@")
SHOW_ALL_IRQS=yes

for interrupt in "${vm_interrupts[@]}"; do
	reset_irqs
	irqs=${interrupt%.*}.irqs
	cpus=${interrupt%.*}.cpus
	[[ -e $irqs ]]
	[[ -e $cpus ]]
	for irq in $(< "$irqs"); do
		irqs_to_lookup[irq]=$irq
	done
	cpus_override=($(< "$cpus"))
	update_irqs_procfs "$interrupt"
	get_irqs "${irqs_to_lookup[@]}" > "$interrupt.parsed"
done
