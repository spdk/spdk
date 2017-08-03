rm -f test.block verify.block
set -xe

DEVICE="/dev/sdb"
BLOCKSIZE=8192
BLOCKCOUNT=$1
RUNS=10000

if [ "$3" = "random" ]; then
	#Create random block
	dd if=/dev/urandom of=test.block bs=$BLOCKSIZE count=$BLOCKCOUNT
else
	#Create block with "FF" data
	dd if=/dev/zero of=test.zero bs=$BLOCKSIZE count=$BLOCKCOUNT
	cat test.zero|tr "\000" "\377" > test.block
fi

if [ "$2" = "short" ]; then
	echo "Write block to sdb..."
	dd if=test.block of=$DEVICE bs=$BLOCKSIZE count=$BLOCKCOUNT &> /dev/null

	for i in $( seq 1 $RUNS ); do
		#echo "Write block to sdb..."
		#dd if=test.block of=$DEVICE bs=$BLOCKSIZE count=$BLOCKCOUNT &> /dev/null
		echo "Read block to sdb... $i"
		dd if=$DEVICE of=verify.block bs=$BLOCKSIZE count=$BLOCKCOUNT &> /dev/null
		cmp test.block verify.block
		if [ "$?" != "0" ]; then
			echo "Compare failed. "
			exit
		else
			echo "Compare successful."
		fi
	done

	exit
fi

echo "Write block to sdb..."
dd if=test.block of=$DEVICE bs=$BLOCKSIZE count=$BLOCKCOUNT &> /dev/null

for i in $( seq 1 $RUNS ); do
	echo "Write block to sdb... $i"
	dd if=test.block of=$DEVICE bs=$BLOCKSIZE count=$BLOCKCOUNT &> /dev/null
done

echo "MUCH WRITE SUCCESS"
for i in $( seq 1 $RUNS ); do
	echo "Read block to sdb... $i"
	dd if=$DEVICE of=verify.block bs=$BLOCKSIZE count=$BLOCKCOUNT &> /dev/null
	cmp test.block verify.block
	if [ "$?" != "0" ]; then
		echo "Compare failed. "
	else
		echo "Compare successful."
	fi
echo "MUCH READ SUCCESS"
done

for i in $( seq 1 $RUNS ); do
	echo "Read block to sdb... $i"
	dd if=$DEVICE of=verify.block bs=$BLOCKSIZE count=$BLOCKCOUNT &> /dev/null
	cmp test.block verify.block
	if [ "$?" != "0" ]; then
		echo "Compare failed. "
	else
		echo "Compare successful."
	fi

	echo "Write block to sdb... $i"
	dd if=test.block of=$DEVICE bs=$BLOCKSIZE count=$BLOCKCOUNT &> /dev/null

	echo "Read block to sdb... $i"
	dd if=$DEVICE of=verify.block bs=$BLOCKSIZE count=$BLOCKCOUNT &> /dev/null
	cmp test.block verify.block
	if [ "$?" != "0" ]; then
		echo "Compare failed. "
	else
		echo "Compare successful."
	fi
echo "MUCH MIXED SUCCESS"
done
