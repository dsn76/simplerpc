
BUILD_TS=$(shell date +'%Y%m%d%H%M%S')

SRC=libsrpc.c libsrpc_daemon.c embedded_loader.c libsrpc_unix_socket.c libsrpc_fixblockalloc.c libsrpc_shmem.c libsrpc_errno.c libsrpc_mpmcq.c libsrpc_sync.c libsrpc_pthread.c libsrpc_dlist.c libsrpc_list.c

all: app
#	@gcc -Wall -o tests libsrpc_test.c libsrpc_fixblockalloc.c
	@echo "BUILD END"

app: libsrpc.so
	@echo "BLD: $@"
	@gcc -Wall -o app ../example/app.c -L../src -lsrpc -Wl,-rpath,../src -I../src -ggdb3 -O0

libsrpc.so: $(SRC) libtlsf.a
	@echo "BLD: $@"
	@gcc -Wall -fPIC -shared -o libsrpc.so -ldl $(SRC) -DBUILD_TS="\"$(BUILD_TS)\"" -I. -I../libs/tlsf/src -L. -ltlsf
# -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

embedded_loader.c: loader.c
	@echo "BLD: $@"
	@gcc -Wall loader.c -static -s -o loader
	@xxd -i loader > embedded_loader.c

libtlsf.a:
	@echo "BLD: $@"
	@gcc -fPIC -O2 -Wall -I. -DTLSF_BUL=1 -c ../libs/tlsf/src/tlsf.c -o ./tlsf.o
	@gcc -fPIC -O2 -Wall -I. -DTLSF_BUL=1 -c ../libs/tlsf/src/bul.c  -o ./bul.o
	@ar rcs $@ ./tlsf.o ./bul.o


clean:
	@echo "CLEAN"
	@rm -f *.o *.a *.so app tests loader embedded_loader.c


test: libsrpc.so
	@echo "TEST"
	@gcc -Wall -o test test.c -L. -lsrpc -Wl,-rpath,./ -ggdb3

