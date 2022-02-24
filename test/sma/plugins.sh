#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

function create_device() {
	"$rootdir/scripts/sma-client.py" <<- EOF
		{
			"method": "CreateDevice",
			"params": {
				"$1": {}
			}
		}
	EOF
}

trap 'killprocess $smapid; exit 1' SIGINT SIGTERM EXIT

# First check a single plugin with both its devices enabled in the config
PYTHONPATH=$testdir/plugins $rootdir/scripts/sma.py -c <(
	cat <<- EOF
		plugins:
		  - 'plugin1'
		devices:
		  - name: 'plugin1-device1'
		  - name: 'plugin1-device2'
	EOF
) &
smapid=$!
# Wait for a while to make sure SMA starts listening
sma_waitforlisten

[[ $(create_device nvme | jq -r '.handle') == 'nvme:plugin1-device1' ]]
[[ $(create_device nvmf_tcp | jq -r '.handle') == 'nvmf_tcp:plugin1-device2' ]]

killprocess $smapid

# Check that it's possible to enable only a single device from a plugin
PYTHONPATH=$testdir/plugins $rootdir/scripts/sma.py -c <(
	cat <<- EOF
		plugins:
		  - 'plugin1'
		devices:
		  - name: 'plugin1-device2'
	EOF
) &
smapid=$!
sma_waitforlisten

[[ $(create_device nvmf_tcp | jq -r '.handle') == 'nvmf_tcp:plugin1-device2' ]]
NOT create_device nvme

killprocess $smapid

# Load two different plugins, but only enable devices from one of them
PYTHONPATH=$testdir/plugins $rootdir/scripts/sma.py -c <(
	cat <<- EOF
		plugins:
		  - 'plugin1'
		  - 'plugin2'
		devices:
		  - name: 'plugin1-device1'
		  - name: 'plugin1-device2'
	EOF
) &
smapid=$!
sma_waitforlisten

[[ $(create_device nvme | jq -r '.handle') == 'nvme:plugin1-device1' ]]
[[ $(create_device nvmf_tcp | jq -r '.handle') == 'nvmf_tcp:plugin1-device2' ]]

killprocess $smapid

# Check the same but take devices defined by the other plugin
PYTHONPATH=$testdir/plugins $rootdir/scripts/sma.py -c <(
	cat <<- EOF
		plugins:
		  - 'plugin1'
		  - 'plugin2'
		devices:
		  - name: 'plugin2-device1'
		  - name: 'plugin2-device2'
	EOF
) &
smapid=$!
sma_waitforlisten

[[ $(create_device nvme | jq -r '.handle') == 'nvme:plugin2-device1' ]]
[[ $(create_device nvmf_tcp | jq -r '.handle') == 'nvmf_tcp:plugin2-device2' ]]

killprocess $smapid

# Now pick a device from each plugin
PYTHONPATH=$testdir/plugins $rootdir/scripts/sma.py -c <(
	cat <<- EOF
		plugins:
		  - 'plugin1'
		  - 'plugin2'
		devices:
		  - name: 'plugin1-device1'
		  - name: 'plugin2-device2'
	EOF
) &
smapid=$!
sma_waitforlisten

[[ $(create_device nvme | jq -r '.handle') == 'nvme:plugin1-device1' ]]
[[ $(create_device nvmf_tcp | jq -r '.handle') == 'nvmf_tcp:plugin2-device2' ]]

killprocess $smapid

# Check the same, but register plugins via a env var
PYTHONPATH=$testdir/plugins SMA_PLUGINS=plugin1:plugin2 $rootdir/scripts/sma.py -c <(
	cat <<- EOF
		devices:
		  - name: 'plugin1-device1'
		  - name: 'plugin2-device2'
	EOF
) &
smapid=$!
sma_waitforlisten

[[ $(create_device nvme | jq -r '.handle') == 'nvme:plugin1-device1' ]]
[[ $(create_device nvmf_tcp | jq -r '.handle') == 'nvmf_tcp:plugin2-device2' ]]

killprocess $smapid

# Finally, register one plugin in a config and the other through env var
PYTHONPATH=$testdir/plugins SMA_PLUGINS=plugin1 $rootdir/scripts/sma.py -c <(
	cat <<- EOF
		plugins:
		  - 'plugin2'
		devices:
		  - name: 'plugin1-device1'
		  - name: 'plugin2-device2'
	EOF
) &
smapid=$!
sma_waitforlisten

[[ $(create_device nvme | jq -r '.handle') == 'nvme:plugin1-device1' ]]
[[ $(create_device nvmf_tcp | jq -r '.handle') == 'nvmf_tcp:plugin2-device2' ]]

killprocess $smapid

trap - SIGINT SIGTERM EXIT
