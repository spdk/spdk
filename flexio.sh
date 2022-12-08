ARCH=`uname -m`

if [[ $ARCH == "aarch64" ]]; then
        echo "ARM"
	LIBPATH=/usr/lib/aarch64-linux-gnu/
else
        echo "x86"
	LIBPATH=/usr/lib/x86_64-linux-gnu/
fi

# Useful constants
COLOR_RED="\033[0;31m"
COLOR_GREEN="\033[0;32m"
COLOR_OFF="\033[0m"

# skip clone if on simx env where sdk already exists
#git clone http://l-gerrit.mtl.labs.mlnx:8080/flexio-sdk

# goto where sdk is manually run the following
#cd flexio-sdk
./build.sh --clean


echo -e "${COLOR_GREEN}flexio has been builded ${COLOR_OFF}"
sleep 1

# copy over build artifacts, headers, shared and/or static libraries to system
# path so that they can be used by virtio dpa stuff
cp -r libflexio /usr/include/
cp -r libflexio-dev /usr/include/
cp -r common /usr/include/
rm -fr /usr/include/libflexio-libc
mkdir /usr/include/libflexio-libc
cp libflexio-libc/*.h /usr/include/libflexio-libc/
rm -fr /usr/include/libflexio-os
mkdir /usr/include/libflexio-os
cp libflexio-os/*.h /usr/include/libflexio-os/
cp ./cc-host/libflexio/libflexio.so $LIBPATH
cp ./clang-target/libflexio-dev/libflexio_dev.a $LIBPATH
cp libflexio-libc/lib/libflexio-libc.a $LIBPATH
cp libflexio-os/lib/libflexio_os.a $LIBPATH
echo -e "${COLOR_GREEN}flexio has been copied ${COLOR_OFF}"
