#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

jsonconf=$testdir/conf.json
testconf=$testdir/test.conf

trap 'cleanup; exit 1' SIGINT SIGTERM EXIT
#Test inheriting filename and rw_mode parameters from global section.
create_job "global" "read" "Malloc0"
create_job "job0"
create_job "job1"
create_job "job2"
create_job "job3"
bdevperf_output=$($bdevperf -t 2 --json $jsonconf -j $testconf 2>&1)
[[ $(get_num_jobs "$bdevperf_output") == "4" ]]

bdevperf_output=$($bdevperf -C -t 2 --json $jsonconf -j $testconf)

cleanup
#Test missing global section.
create_job "job0" "write" "Malloc0"
create_job "job1" "write" "Malloc0"
create_job "job2" "write" "Malloc0"
bdevperf_output=$($bdevperf -t 2 --json $jsonconf -j $testconf 2>&1)
[[ $(get_num_jobs "$bdevperf_output") == "3" ]]

cleanup
#Test inheriting multiple filenames and rw_mode parameters from global section.
create_job "global" "rw" "Malloc0:Malloc1"
create_job "job0"
create_job "job1"
create_job "job2"
create_job "job3"
bdevperf_output=$($bdevperf -t 2 --json $jsonconf -j $testconf 2>&1)
[[ $(get_num_jobs "$bdevperf_output") == "4" ]]
cleanup
trap - SIGINT SIGTERM EXIT
