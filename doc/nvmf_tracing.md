# NVMe-oF Target Tracepoints {#nvmf_tgt_tracepoints}

# Introduction {#tracepoints_intro}

Tracepoints provide a high-performance tracing mechanism that is accessible at
runtime. They are implemented as a circular buffer in shared memory that is
accessible from other processes. They can be used to inspect the behavior of an
SPDK application without stopping the application. For example, you can inspect
the behavior of the SPDK target application using tracepoints at different points
in the RDMA state machine to log information to a debug window. Analyzing the
events logged in the debug window can provide insights such as the amount of
time I/Os spend in different RDMA states.

# Enabling Tracepoints {#enable_tracepoints}

Tracepoints are placed in groups. They are enabled and disabled as a group.
To enable all the tracepoints group in an SPDK target application set the
TpointGroupMask to 0xFFFF in the Global section of the configuration file.
To enable all the RDMA tracepoints in an SPDK target application, set the
TpointGroupMask to 0x10 in the Global section of the configuration file.

~~~
[Global]
  ...
  TpointGroupMask 0x10
  ...
~~~

The TpointGroupMask can also be specified at the command line using -e
option as shown below.

~~~
app/spdk_tgt/spdk_tgt -c nvmf.conf -e 0x10
Starting SPDK v18.07-pre / DPDK 18.02.0 initialization...
[ DPDK EAL parameters: spdk_tgt -c 0x1 --file-prefix=spdk_pid2576 ]
EAL: Detected 24 lcore(s)
EAL: No free hugepages reported in hugepages-1048576kB
EAL: Multi-process socket /var/run/.spdk_pid2576_unix
EAL: Probing VFIO support...
app.c: 521:spdk_app_start: *NOTICE*: Total cores available: 1
reactor.c: 706:spdk_reactors_init: *NOTICE*: Occupied cpu socket mask is 0x1
app.c: 459:spdk_app_setup_trace: *NOTICE*: Tracepoint Group Mask 0xFF specified.
app.c: 463:spdk_app_setup_trace: *NOTICE*: Use 'spdk_trace -s spdk_tgt -p 2576' to capture a snapshot of events at runtime.
~~~

When the target starts, a message is logged with the information you need to view
the tracepoints in a human-readable format such as the name of the process and
the process id.

# Capturing a snapshot of events {#capture_tracepoints}

Send I/Os to the SPDK target application to generate events. The following is
an example usage of perf to send I/Os to the NVMe-oF target over an RDMA network
interface for 10 minutes.

~~~
./perf -q 128 -s 4096 -w randread -t 600 -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.2 trsvcid:4420'
~~~

Use the spdk_trace application on the target system to capture a snapshot of
events while the target is processing I/Os.

~~~
app/trace/spdk_trace -s spdk_tgt -p 2576
~~~

The parameters you need to run the spdk_trace application are logged by the NVMe-oF
target when it starts. Look for a message such as 'spdk_trace -s spdk_tgt -p 2576'
in the logged messages.

The following is sample trace capture showing the cumulative time that each
I/O spends at each RDMA state. All the trace captures with the same id are for
the same I/O.

~~~
28:   6026.658 ( 12656064)     RDMA_REQ_NEED_BUFFER                                      id:    r3622            time:  0.019
28:   6026.694 ( 12656140)     RDMA_REQ_RDY_TO_EXECUTE                                   id:    r3622            time:  0.055
28:   6026.820 ( 12656406)     RDMA_REQ_EXECUTING                                        id:    r3622            time:  0.182
28:   6026.992 ( 12656766)     RDMA_REQ_EXECUTED                                         id:    r3477            time:  228.510
28:   6027.010 ( 12656804)     RDMA_REQ_TX_PENDING_C_TO_H                                id:    r3477            time:  228.528
28:   6027.022 ( 12656828)     RDMA_REQ_RDY_TO_COMPLETE                                  id:    r3477            time:  228.539
28:   6027.115 ( 12657024)     RDMA_REQ_COMPLETING                                       id:    r3477            time:  228.633
28:   6027.471 ( 12657770)     RDMA_REQ_COMPLETED                                        id:    r3518            time:  171.577
28:   6028.027 ( 12658940)     RDMA_REQ_NEW                                              id:    r3623
28:   6028.057 ( 12659002)     RDMA_REQ_NEED_BUFFER                                      id:    r3623            time:  0.030
28:   6028.095 ( 12659082)     RDMA_REQ_RDY_TO_EXECUTE                                   id:    r3623            time:  0.068
28:   6028.216 ( 12659336)     RDMA_REQ_EXECUTING                                        id:    r3623            time:  0.189
28:   6028.408 ( 12659740)     RDMA_REQ_EXECUTED                                         id:    r3505            time:  190.509
28:   6028.441 ( 12659808)     RDMA_REQ_TX_PENDING_C_TO_H                                id:    r3505            time:  190.542
28:   6028.452 ( 12659832)     RDMA_REQ_RDY_TO_COMPLETE                                  id:    r3505            time:  190.553
28:   6028.536 ( 12660008)     RDMA_REQ_COMPLETING                                       id:    r3505            time:  190.637
28:   6028.854 ( 12660676)     RDMA_REQ_COMPLETED                                        id:    r3465            time:  247.000
28:   6029.433 ( 12661892)     RDMA_REQ_NEW                                              id:    r3624
28:   6029.452 ( 12661932)     RDMA_REQ_NEED_BUFFER                                      id:    r3624            time:  0.019
28:   6029.482 ( 12661996)     RDMA_REQ_RDY_TO_EXECUTE                                   id:    r3624            time:  0.050
28:   6029.591 ( 12662224)     RDMA_REQ_EXECUTING                                        id:    r3624            time:  0.158
28:   6029.782 ( 12662624)     RDMA_REQ_EXECUTED                                         id:    r3564            time:  96.937
28:   6029.798 ( 12662658)     RDMA_REQ_TX_PENDING_C_TO_H                                id:    r3564            time:  96.953
28:   6029.812 ( 12662688)     RDMA_REQ_RDY_TO_COMPLETE                                  id:    r3564            time:  96.967
28:   6029.899 ( 12662870)     RDMA_REQ_COMPLETING                                       id:    r3564            time:  97.054
28:   6030.262 ( 12663634)     RDMA_REQ_COMPLETED                                        id:    r3477            time:  231.780
28:   6030.786 ( 12664734)     RDMA_REQ_NEW                                              id:    r3625
28:   6030.804 ( 12664772)     RDMA_REQ_NEED_BUFFER                                      id:    r3625            time:  0.018
28:   6030.841 ( 12664848)     RDMA_REQ_RDY_TO_EXECUTE                                   id:    r3625            time:  0.054
28:   6030.963 ( 12665104)     RDMA_REQ_EXECUTING                                        id:    r3625            time:  0.176
28:   6031.139 ( 12665474)     RDMA_REQ_EXECUTED                                         id:    r3552            time:  114.906
28:   6031.196 ( 12665594)     RDMA_REQ_TX_PENDING_C_TO_H                                id:    r3552            time:  114.963
28:   6031.210 ( 12665624)     RDMA_REQ_RDY_TO_COMPLETE                                  id:    r3552            time:  114.977
28:   6031.293 ( 12665798)     RDMA_REQ_COMPLETING                                       id:    r3552            time:  115.060
28:   6031.633 ( 12666512)     RDMA_REQ_COMPLETED                                        id:    r3505            time:  193.734
28:   6032.230 ( 12667766)     RDMA_REQ_NEW                                              id:    r3626
28:   6032.248 ( 12667804)     RDMA_REQ_NEED_BUFFER                                      id:    r3626            time:  0.018
28:   6032.288 ( 12667888)     RDMA_REQ_RDY_TO_EXECUTE                                   id:    r3626            time:  0.058
28:   6032.396 ( 12668114)     RDMA_REQ_EXECUTING                                        id:    r3626            time:  0.166
28:   6032.593 ( 12668528)     RDMA_REQ_EXECUTED                                         id:    r3570            time:  90.443
28:   6032.611 ( 12668564)     RDMA_REQ_TX_PENDING_C_TO_H                                id:    r3570            time:  90.460
28:   6032.623 ( 12668590)     RDMA_REQ_RDY_TO_COMPLETE                                  id:    r3570            time:  90.473
28:   6032.707 ( 12668766)     RDMA_REQ_COMPLETING                                       id:    r3570            time:  90.557
28:   6033.056 ( 12669500)     RDMA_REQ_COMPLETED                                        id:    r3564            time:  100.211
~~~

# Adding New Tracepoints {#add_tracepoints}

SPDK applications and libraries provide several trace points. For example,
the SPDK RDMA library (lib/nvmf/rdma.c) provides the following tracepoints.

~~~
#define	TRACE_GROUP_NVMF_RDMA	0x4
#define TRACE_RDMA_REQUEST_STATE_NEW	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x0)
#define TRACE_RDMA_REQUEST_STATE_NEED_BUFFER	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x1)
#define TRACE_RDMA_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x2)
#define TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x3)
#define TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x4)
#define TRACE_RDMA_REQUEST_STATE_EXECUTING	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x5)
#define TRACE_RDMA_REQUEST_STATE_EXECUTED	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x6)
#define TRACE_RDMA_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x7)
#define TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x8)
#define TRACE_RDMA_REQUEST_STATE_COMPLETING	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0x9)
#define TRACE_RDMA_REQUEST_STATE_COMPLETED	SPDK_TPOINT_ID(TRACE_GROUP_NVMF_RDMA, 0xA)
~~~

You can define new tracepoints by adding them to the trace group
(like TRACE_GROUP_NVMF_RDMA above) and assigning them a unique ID using the
SPDK_TPOINT_ID macro. You also need to register the new trace points in the
SPDK_TRACE_REGISTER_FN macro call within the application/library using the
spdk_trace_register_description function as shown below.

~~~
SPDK_TRACE_REGISTER_FN(nvmf_trace)
{
	spdk_trace_register_object(OBJECT_NVMF_RDMA_IO, 'r');
	spdk_trace_register_description("RDMA_REQ_NEW", "",
					TRACE_RDMA_REQUEST_STATE_NEW,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 1, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_NEED_BUFFER", "",
					TRACE_RDMA_REQUEST_STATE_NEED_BUFFER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_H_TO_C", "",
					TRACE_RDMA_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_TX_H_TO_C", "",
					TRACE_RDMA_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_EXECUTE", "",
					TRACE_RDMA_REQUEST_STATE_READY_TO_EXECUTE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_EXECUTING", "",
					TRACE_RDMA_REQUEST_STATE_EXECUTING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_EXECUTED", "",
					TRACE_RDMA_REQUEST_STATE_EXECUTED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_TX_PENDING_C_TO_H", "",
					TRACE_RDMA_REQUEST_STATE_TRANSFER_PENDING_CONTROLLER_TO_HOST,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_RDY_TO_COMPLETE", "",
					TRACE_RDMA_REQUEST_STATE_READY_TO_COMPLETE,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_COMPLETING", "",
					TRACE_RDMA_REQUEST_STATE_COMPLETING,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
	spdk_trace_register_description("RDMA_REQ_COMPLETED", "",
					TRACE_RDMA_REQUEST_STATE_COMPLETED,
					OWNER_NONE, OBJECT_NVMF_RDMA_IO, 0, 0, 0, "");
}
~~~

Finally, use the spdk_trace_record function at the appropriate point in the
application/library to record the current trace state for the new trace points.
The following example shows the usage of the spdk_trace_record function to
record the current trace state of several tracepoints.

~~~
	case RDMA_REQUEST_STATE_NEW:
		spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEW, 0, 0, (uintptr_t)rdma_req, 0);
		...
		break;
	case RDMA_REQUEST_STATE_NEED_BUFFER:
		spdk_trace_record(TRACE_RDMA_REQUEST_STATE_NEED_BUFFER, 0, 0, (uintptr_t)rdma_req, 0);
		...
		break;
	case RDMA_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER:
		spdk_trace_record(TRACE_RDMA_REQUEST_STATE_TRANSFER_PENDING_HOST_TO_CONTROLLER, 0, 0,
			(uintptr_t)rdma_req, 0);
		...
~~~

All the tracing functions are documented in the [Tracepoint library documentation](http://www.spdk.io/doc/trace_8h.html)
