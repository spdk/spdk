The blobcli tool has several options that are listed by using the -h command
however the three operating modes are covered in more detail here:

Command Mode
------------

This is the default and will just execute one command at a time. It's simple
but the downside is that if you are going to interact quite a bit with the
blobstore, the startup time for the application can be cumbersome.

Shell Mode
----------

You startup shell mode by using the -S command. At that point you will get
a "blob>" prompt where you can enter any of the commands, including -h,
to execute them. You can stil enter just one at a time but the initial
startup time for the application will not get in the way between commands
anymore so it is much more usable.

Script (aka test) Mode
----------------------

In script mode you just supply one command with a filename when you start
the cli, for example `blobcli -T test.bs` will feed the tool the file
called test.bs which contains a series of commands that will all run
automatically and, like shell mode, will only initialize one time so is
quick.

The script file format (example) is shown below.  Comments are allowed and
each line should contain one valid command (and its parameters) only. In
order to operate on blobs via their ID value, use the token $Bn where n
represents the instance of the blob created in the script.

For example, the line `-s $B0` will operate on the blobid of the first
blob created in the script (0 index based). `$B2` represents the third
blob created in the script.

If you start test mode with the additional "ignore" option, any invalid
script lines will simply be skipped, otherwise the tool will exit if
it runs into an invalid line (ie './blobcli -T test.bs ignore`).

Sample test/bs file:

~~~{.sh}
# this is a comment
-i
-s bs
-l bdevs
-n 1
-s bs
-s $B0
-n 2
-s $B1
-m $B0 Makefile
-d $B0 M.blob
-f $B1 65
-d $B1 65.blob
-s bs
-x $B0 b0key boval
-x $B1 b1key b1val
-r $B0 b0key
-s $B0
-s $B1
-s bs
~~~
