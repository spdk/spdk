#!/usr/bin/env bash
set -x

rootdir=$(readlink -f $(dirname $0))

# we can add some actions to clean up files which no need anymore, such as share memory overflow...
if [ `uname` = Linux ] ;then
        echo "Delete leftover SPDK generated files from terminated processes."
	# serach sensible file
	for file in `ls /dev/shm/* |grep trace.pid`
        do
	   #confirm is a file	
           if test -f $file
           then
               FileName=`echo $file`
               PID=`echo ${FileName#*trace.pid}`
               #confirm PID is a  number
               if [ -n "$(echo $PID| sed -n "/^[0-9]\+$/p")" ] ;then
                        
	    	    if [[ $PID -eq 0 ]] ;then
                        continue
               	    else
                        kill -0 $PID
                        #Found the target PID
                        if [[ $? -eq 0 ]] ;then
                                echo "Target $PID is running, skip deleting this file."
                        else
                                # Delete leftover SPDK generated files from terminated processes.
                                echo "Delete trace.pid$PID file"
                                rm -rf /dev/shm/*trace.pid$PID
                                # only one spdk_iscsi_conns file, no need to delete.
                                #rm -rf /dev/shm/spdk_iscsi_conns.-1
                        fi
                    fi
		else
		#skip non number PID	
			continue
                fi
	    fi
        done
elif [ `uname` =  FreeBSD ] ;then
        echo " FreeBSD need to add extra cleanup behavior check !"
else
        echo "Not support!!"
fi
