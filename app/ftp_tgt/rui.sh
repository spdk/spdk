
SPDK_ROOT=/root/spdk
cd ${SPDK_ROOT}/lib/ftp
make
cd ${SPDK_ROOT}/lib/event/subsystems/ftp
make
cd ${SPDK_ROOT}/app/ftp_tgt
make
