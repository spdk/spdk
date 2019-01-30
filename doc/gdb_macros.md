# GDB Macros User Guide {#gdb_macros}

# Introduction

When debugging an spdk application using gdb we may need to view data structures
in lists, e.g. information about bdevs or threads.

If, for example I have several bdevs, and I with to get information on bdev by
the name 'test_vols3', I will need to manually iterate over the list as follows:

~~~{.sh}
(gdb) p g_bdev_mgr->bdevs->tqh_first->name
$5 = 0x7f7dcc0b21b0 "test_vols1"
(gdb) p g_bdev_mgr->bdevs->tqh_first->internal->link->tqe_next->name
$6 = 0x7f7dcc0b1a70 "test_vols2"
(gdb) p
g_bdev_mgr->bdevs->tqh_first->internal->link->tqe_next->internal->link->tqe_next->name
$7 = 0x7f7dcc215a00 "test_vols3"
(gdb) p
g_bdev_mgr->bdevs->tqh_first->internal->link->tqe_next->internal->link->tqe_next
$8 = (struct spdk_bdev *) 0x7f7dcc2c7c08
~~~

At this stage, we can start looking at the relevant fields of our bdev which now
we know is in address 0x7f7dcc2c7c08.

This can be somewhat troublesome if there are 100 bdevs, and the one we need is
56th in the list...

Instead, we can use a gdb macro in order to get information about all the
devices:

~~~{.sh}
(gdb) spdk_print_bdevs

SPDK object of type struct spdk_bdev at 0x7f7dcc1642a8
((struct spdk_bdev*) 0x7f7dcc1642a8)
name 0x7f7dcc0b21b0 "test_vols1"

---------------

SPDK object of type struct spdk_bdev at 0x7f7dcc216008
((struct spdk_bdev*) 0x7f7dcc216008)
name 0x7f7dcc0b1a70 "test_vols2"

---------------

SPDK object of type struct spdk_bdev at 0x7f7dcc2c7c08
((struct spdk_bdev*) 0x7f7dcc2c7c08)
name 0x7f7dcc215a00 "test_vols3"

---------------
~~~

We can also get gdb to search for the devices for us:

~~~{.sh}
(gdb) spdk_find_bdev test_vols1
test_vols1

SPDK object of type struct spdk_bdev at 0x7f7dcc1642a8
((struct spdk_bdev*) 0x7f7dcc1642a8)
name 0x7f7dcc0b21b0 "test_vols1"
~~~

# Loading The gdb Macros

Copy the gdb macros to the host where you are about to debug.
It is best to copy the file either to somewhere within the PYTHONPATH, or to add
the destination directory to the PYTHONPATH. This is not mandatory, and can be
worked around, but can save a few steps when loading the module to gdb.

From gdb, with the application core open, invoke python and load the modules.

In the example bellow, I copied the macros to the /tmp directory which is not in
the PYTHONPATH, so I had to manually add the directory to the path.

~~~{.sh}
(gdb) python
>import sys
>sys.path.append('/tmp')
>import gdb_macros
>end
(gdb) spdk_load_macros
~~~

# Using .gdbinit To Load The Macros

The steps above can be automated usind a .gdbinit file which saves you manually
importing the macros.

Exmaple .gdbinit:

~~~{.py}
python
import sys
sys.path.append('/opt/km/install/tools/gdb_macros/')
import gdb_macros
end
~~~

When starting gdb you still have to call spdk_load_macros.

# Why Do We Need to Explicitly Call spdk_load_macros

The reason is that the macros need to use globals provided by spdk in order to
iterate the spdk lists and build iterable representations of the list objects.
This will result in errors if these are not available which is very possible if
gdb is used for reasons other than debugging spdk core dumps.

In the example bellow, I attempted to load the macros when the globals are not
available causing gdb to fail loading the gdb_macros:

~~~{.sh}
(gdb) spdk_load_macros
Traceback (most recent call last):
  File "/opt/km/install/tools/gdb_macros/gdb_macros.py", line 257, in invoke
    spdk_print_threads()
  File "/opt/km/install/tools/gdb_macros/gdb_macros.py", line 241, in __init__
    threads = SpdkThreads()
  File "/opt/km/install/tools/gdb_macros/gdb_macros.py", line 234, in __init__
    super(SpdkThreads, self).__init__('g_threads', SpdkThread)
  File "/opt/km/install/tools/gdb_macros/gdb_macros.py", line 25, in __init__
    ['tailq'])
  File "/opt/km/install/tools/gdb_macros/gdb_macros.py", line 10, in __init__
    self.list = gdb.parse_and_eval(self.list_pointer)
RuntimeError: No symbol table is loaded.  Use the "file" command.
Error occurred in Python command: No symbol table is loaded.  Use the "file"
command.
~~~

# Macros available:

- spdk_load_macros: load the macros (use --reload in order to reload them)
- spdk_print_bdevs: information about bdevs
- spdk_find_bdev: find a bdev (substring search)
- spdk_print_io_devices: information about io devices
- spdk_print_nvmf_subsystems: information about nvmf subsystems
- spdk_print_threads: information about threads

# Adding New Macros:

The list iteration macros are usually built from 3 layers:

- SpdkPrintCommand: inherits from gdb.Command and invokes the list iteration
- SpdkTailqList: Performs the iteration of a tailq list according to the tailq
  member implementation
- SpdkObject: Provides the __str__ function so that the list iteration can print
  the object

Other useful objects:

- SpdkNormalTailqList: represents a list which has 'tailq' as the tailq object
- SpdkArr: Iteration over an array (instead of a linked list)
