function sma_waitforlisten() {
	local sma_addr=${1:-127.0.0.1}
	local sma_port=${2:-8080}

	for ((i = 0; i < 5; i++)); do
		if nc -z $sma_addr $sma_port; then
			return 0
		fi
		sleep 1s
	done
	return 1
}

function uuid2base64() {
	python <<- EOF
		import base64, uuid
		print(base64.b64encode(uuid.UUID("$1").bytes).decode())
	EOF
}
