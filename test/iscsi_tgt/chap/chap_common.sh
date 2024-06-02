#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#

TARGET_NAME="iqn.2016-06.io.spdk:disk1"
TARGET_ALIAS_NAME="disk1_alias"
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

function parse_cmd_line() {
	OPTIND=0
	DURING_DISCOVERY=0
	DURING_LOGIN=0
	BI_DIRECT=0
	CHAP_USER="chapo"
	CHAP_PASS="123456789123"
	CHAP_MUSER=""
	CHAP_MUSER=""
	AUTH_GROUP_ID=1

	while getopts ":t:u:s:r:m:dlb" opt; do
		case ${opt} in
			t)
				AUTH_GROUP_ID=$OPTARG
				;;
			u)
				CHAP_USER=$OPTARG
				;;
			s)
				CHAP_PASS=$OPTARG
				;;
			r)
				CHAP_MUSER=$OPTARG
				;;
			m)
				CHAP_MPASS=$OPTARG
				;;
			d)
				DURING_DISCOVERY=1
				;;
			l)
				DURING_LOGIN=1
				;;
			b)
				BI_DIRECT=1
				;;
			\?)
				echo "Usage: config_chap_credentials_for_target/config_chap_credentials_for_initiator [-t auth_group id] \
            [-u user] [-s password] [-r muser] [-m mpassword] [-d] [-l] [-b]"
				;;
		esac
	done
}

function restart_iscsid() {
	sleep 3
	systemctl restart iscsid
	sleep 1
}

function default_initiator_chap_credentials() {
	iscsiadm -m node --logout || true
	iscsiadm -m node -o delete || true

	sed -i "s/^node.session.auth.authmethod = CHAP/#node.session.auth.authmethod = CHAP/" /etc/iscsi/iscsid.conf
	sed -i "s/^node.session.auth.username =.*/#node.session.auth.username = username/" /etc/iscsi/iscsid.conf
	sed -i "s/^node.session.auth.password =.*/#node.session.auth.password = password/" /etc/iscsi/iscsid.conf
	sed -i "s/^node.session.auth.username_in =.*/#node.session.auth.username_in = username_in/" /etc/iscsi/iscsid.conf
	sed -i "s/^node.session.auth.password_in =.*/#node.session.auth.password_in = password_in/" /etc/iscsi/iscsid.conf

	sed -i 's/^discovery.sendtargets.auth.authmethod = CHAP/#discovery.sendtargets.auth.authmethod = CHAP/' /etc/iscsi/iscsid.conf
	sed -i 's/^discovery.sendtargets.auth.username =.*/#discovery.sendtargets.auth.username = username/' /etc/iscsi/iscsid.conf
	sed -i 's/^discovery.sendtargets.auth.password =.*/#discovery.sendtargets.auth.password = password/' /etc/iscsi/iscsid.conf
	sed -i "s/^discovery.sendtargets.auth.username_in =.*/#discovery.sendtargets.auth.username_in = username_in/" /etc/iscsi/iscsid.conf
	sed -i "s/^discovery.sendtargets.auth.password_in =.*/#discovery.sendtargets.auth.password_in = password_in/" /etc/iscsi/iscsid.conf
	restart_iscsid
	trap "trap - ERR; print_backtrace >&2" ERR
}

function config_chap_credentials_for_target() {

	parse_cmd_line "$@"
	#create auth group $AUTH_GROUP_ID
	$rpc_py iscsi_create_auth_group $AUTH_GROUP_ID
	#add secret + msecret to the auth group
	if [ -z "$CHAP_MUSER" ] || [ -z "$CHAP_MPASS" ]; then
		$rpc_py iscsi_auth_group_add_secret -u $CHAP_USER -s $CHAP_PASS $AUTH_GROUP_ID
	else
		$rpc_py iscsi_auth_group_add_secret -u $CHAP_USER -s $CHAP_PASS -m $CHAP_MUSER -r $CHAP_MPASS $AUTH_GROUP_ID
	fi

	#set chap authentication method during discovery phase
	if [ $DURING_LOGIN -eq 1 ]; then
		if [ $BI_DIRECT -eq 1 ]; then
			$rpc_py iscsi_target_node_set_auth -g $AUTH_GROUP_ID -r -m $TARGET_NAME
		else
			$rpc_py iscsi_target_node_set_auth -g $AUTH_GROUP_ID -r $TARGET_NAME
		fi
	fi
	if [ $DURING_DISCOVERY -eq 1 ]; then
		if [ $BI_DIRECT -eq 1 ]; then
			$rpc_py iscsi_set_discovery_auth -r -m -g $AUTH_GROUP_ID
		else
			$rpc_py iscsi_set_discovery_auth -r -g $AUTH_GROUP_ID
		fi
	fi
}

function config_chap_credentials_for_initiator() {

	parse_cmd_line "$@"
	default_initiator_chap_credentials

	if [ $DURING_LOGIN -eq 1 ]; then
		sed -i "s/#node.session.auth.authmethod = CHAP/node.session.auth.authmethod = CHAP/" /etc/iscsi/iscsid.conf
		sed -i "s/#node.session.auth.username =.*/node.session.auth.username = ${CHAP_USER}/" /etc/iscsi/iscsid.conf
		sed -i "s/#node.session.auth.password =.*/node.session.auth.password = ${CHAP_PASS}/" /etc/iscsi/iscsid.conf
		if [ $BI_DIRECT -eq 1 ] && [ -n "$CHAP_MPASS" ] && [ -n "$CHAP_MUSER" ]; then
			sed -i "s/#node.session.auth.username_in =.*/node.session.auth.username_in = ${CHAP_MUSER}/" /etc/iscsi/iscsid.conf
			sed -i "s/#node.session.auth.password_in =.*/node.session.auth.password_in = ${CHAP_MPASS}/" /etc/iscsi/iscsid.conf
		fi
	fi

	if [ $DURING_DISCOVERY -eq 1 ]; then
		sed -i "s/#discovery.sendtargets.auth.authmethod = CHAP/discovery.sendtargets.auth.authmethod = CHAP/" /etc/iscsi/iscsid.conf
		sed -i "s/#discovery.sendtargets.auth.username =.*/discovery.sendtargets.auth.username = ${CHAP_USER}/" /etc/iscsi/iscsid.conf
		sed -i "s/#discovery.sendtargets.auth.password =.*/discovery.sendtargets.auth.password = ${CHAP_PASS}/" /etc/iscsi/iscsid.conf
		if [ $BI_DIRECT -eq 1 ] && [ -n "$CHAP_MPASS" ] && [ -n "$CHAP_MUSER" ]; then
			sed -i "s/#discovery.sendtargets.auth.username_in =.*/discovery.sendtargets.auth.username_in = ${CHAP_MUSER}/" /etc/iscsi/iscsid.conf
			sed -i "s/#discovery.sendtargets.auth.password_in =.*/discovery.sendtargets.auth.password_in = ${CHAP_MPASS}/" /etc/iscsi/iscsid.conf
		fi
	fi
	restart_iscsid
	trap "trap - ERR; default_initiator_chap_credentials; print_backtrace >&2" ERR
}

function set_up_iscsi_target() {
	timing_enter start_iscsi_tgt
	"${ISCSI_APP[@]}" -m 0x2 -p 1 -s 512 --wait-for-rpc &
	pid=$!
	echo "iSCSI target launched. pid: $pid"
	trap 'killprocess $pid;exit 1' SIGINT SIGTERM EXIT
	waitforlisten $pid
	$rpc_py iscsi_set_options -o 30 -a 4
	$rpc_py framework_start_init
	echo "iscsi_tgt is listening. Running tests..."
	timing_exit start_iscsi_tgt

	$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
	$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
	$rpc_py iscsi_create_target_node $TARGET_NAME $TARGET_ALIAS_NAME 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
	sleep 1
	trap 'killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT
}
