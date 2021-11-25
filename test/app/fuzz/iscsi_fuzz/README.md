# Overview

This application is intended to fuzz test the iSCSI target by submitting
randomized PDU commands through a simulated iSCSI initiator.

## Input

1. iSCSI initiator send a login request PDU to iSCSI Target. Once a session is connected,
2. iSCSI initiator send huge amount and random PDUs continuously to iSCSI Target.
3. iSCSI initiator send a logout request PDU to iSCSI Target in the end.

Especially, iSCSI initiator need to build different bhs according to different bhs opcode.
Then iSCSI initiator will receive all kinds of response opcodes from iSCSI Target.
The application will terminate when run time expires (see the -t flag).

## Output

By default, the fuzzer will print commands that:

1. Complete successfully back from the target, or
2. Are outstanding at the time of a connection error occurs.

Commands are dumped as named objects in json format which can then be supplied back to the
script for targeted debugging on a subsequent run.

At the end of each test run, a summary is printed in the following format:

~~~bash
device 0x11c3b90 stats: Sent 1543 valid opcode PDUs, 16215 invalid opcode PDUs.
~~~
