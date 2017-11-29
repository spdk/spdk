if [ -z $TARGET_IP ]; then
	TARGET_IP=127.0.0.1
	echo "TARGET_IP not defined in environment; defaulting to $TARGET_IP"
fi

if [ -z $INITIATOR_IP ]; then
	INITIATOR_IP=127.0.0.1
	echo "INITIATOR_IP not defined in environment; defaulting to $INITIATOR_IP"
fi

if [ -z $ISCSI_PORT ]; then
	ISCSI_PORT=3260
	echo "ISCSI_PORT not defined in environment; defaulting to $ISCSI_PORT"
fi

if [ -z "$ISCSI_APP" ]; then
	ISCSI_APP=./app/iscsi_tgt/iscsi_tgt
fi

if [ -z "$ISCSI_TEST_CORE_MASK" ]; then
	ISCSI_TEST_CORE_MASK=0xFFFF
fi
