src=vbdev_passthru_rpc.c vbdev_passthru.c
shared:
	gcc $(COMMON_CFLAGS) -c -fPIC ./vbdev_passthru_rpc.c -o ./vbdev_passthru_rpc.o
	gcc $(COMMON_CFLAGS) -c -fPIC ./vbdev_passthru.c -o ./vbdev_passthru.o
	gcc $(COMMON_CFLAGS) -shared ./vbdev_passthru_rpc.o ./vbdev_passthru.o -o ./libpassthru_external.so

static:
	gcc $(COMMON_CFLAGS) -c ./vbdev_passthru_rpc.c -o ./vbdev_passthru_rpc.o
	gcc $(COMMON_CFLAGS) -c ./vbdev_passthru.c -o ./vbdev_passthru.o
	ar rcs ./libpassthru_external.a ./vbdev_passthru_rpc.o ./vbdev_passthru.o

