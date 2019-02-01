# Common shell utility functions

# Check if PCI device is on PCI_WHITELIST and not on PCI_BLACKLIST
# Env:
# if PCI_WHITELIST is empty assume device is whitelistened
# if PCI_BLACKLIST is empty assume device is NOT blacklistened
# Params:
# $1 - PCI BDF
function pci_can_use() {
	local i

	# The '\ ' part is important
	if [[ " $PCI_BLACKLIST " =~ \ $1\  ]] ; then
		return 1
	fi

	if [[ -z "$PCI_WHITELIST" ]]; then
		#no whitelist specified, bind all devices
		return 0
	fi

	for i in $PCI_WHITELIST; do
		if [ "$i" == "$1" ] ; then
			return 0
		fi
	done

	return 1
}

# This function will ignore PCI PCI_WHITELIST and PCI_BLACKLIST
function iter_all_pci_class_code() {
	local class="$(printf %02x $((0x$1)))"
	local subclass="$(printf %02x $((0x$2)))"
	local progif="$(printf %02x $((0x$3)))"

	if hash lspci &>/dev/null; then
		if [ "$progif" != "00" ]; then
			lspci -mm -n -D | \
				grep -i -- "-p${progif}" | \
				awk -v cc="\"${class}${subclass}\"" -F " " \
				'{if (cc ~ $2) print $1}' | tr -d '"'
		else
			lspci -mm -n -D | \
				awk -v cc="\"${class}${subclass}\"" -F " " \
				'{if (cc ~ $2) print $1}' | tr -d '"'
		fi
	elif hash pciconf &>/dev/null; then
		local addr=($(pciconf -l | grep -i "class=0x${class}${subclass}${progif}" | \
			cut -d$'\t' -f1 | sed -e 's/^[a-zA-Z0-9_]*@pci//g' | tr ':' ' '))
		printf "%04x:%02x:%02x:%x\n" ${addr[0]} ${addr[1]} ${addr[2]} ${addr[3]}
	else
		echo "Missing PCI enumeration utility"
		exit 1
	fi
}

# This function will ignore PCI PCI_WHITELIST and PCI_BLACKLIST
function iter_all_pci_dev_id() {
	local ven_id="$(printf %04x $((0x$1)))"
	local dev_id="$(printf %04x $((0x$2)))"

	if hash lspci &>/dev/null; then
		lspci -mm -n -D | awk -v ven="\"$ven_id\"" -v dev="\"${dev_id}\"" -F " " \
			'{if (ven ~ $3 && dev ~ $4) print $1}' | tr -d '"'
	elif hash pciconf &>/dev/null; then
		local addr=($(pciconf -l | grep -i "chip=0x${dev_id}${ven_id}" | \
			cut -d$'\t' -f1 | sed -e 's/^[a-zA-Z0-9_]*@pci//g' | tr ':' ' '))
		printf "%04x:%02x:%02x:%x\n" ${addr[0]} ${addr[1]} ${addr[2]} ${addr[3]}
	else
		echo "Missing PCI enumeration utility"
		exit 1
	fi
}

function iter_pci_dev_id() {
	local bdf=""

	for bdf in $(iter_all_pci_dev_id "$@"); do
		if pci_can_use "$bdf"; then
			echo "$bdf"
		fi
	done
}

# This function will filter out PCI devices using PCI_WHITELIST and PCI_BLACKLIST
# See function pci_can_use()
function iter_pci_class_code() {
	local bdf=""

	for bdf in $(iter_all_pci_class_code "$@"); do
		if pci_can_use "$bdf"; then
			echo "$bdf"
		fi
	done
}
