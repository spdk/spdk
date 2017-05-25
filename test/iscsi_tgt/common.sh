if [ -z $TARGET_IP ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z $INITIATOR_IP ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

if [ -z "$ISCSI_APP" ]; then
	ISCSI_APP=./app/iscsi_tgt/iscsi_tgt
fi

if [ -z "$ISCSI_TEST_CORE_MASK" ]; then
	ISCSI_TEST_CORE_MASK=0xFFFF
fi
