# Makefile

CC = gcc
CXX = g++
AR = ar

#for debug
#CFLAGS = -g -O0 -fPIC -shared -Wall -Werror
#CFLAGS_TEST = -g -O0 -Wall -Werror
#for release
CFLAGS = -g -O3 -shared -Wall -ftls-model=initial-exec -D_GNU_SOURCE
CFLAGS_TEST = -g -O3 -Wall -Werror

TARGET_LIB = libmyth.so libmyth-compat.so libmyth-native.so  
TARGET_TEST = 
TARGET = $(TARGET_LIB) $(TARGET_TEST)

CFLAGS += -fPIC -DCOMPILED_AS_PIC

all: prepare $(TARGET)

prepare: myth.d

# Suffix rules
.SUFFIXES: .o .c
.c.o:
	$(CC) $(CFLAGS) -c $<

.SUFFIXES: .o .S
.S.o:
	$(CC) $(CFLAGS) -c $<

myth.d: pthread_so_path.def
	gcc -MM -w *.c > myth.d

sinclude myth.d

# Shared library path
pthread_so_path.def:
	gcc -o search_shlib_path search_shlib_path.c -lpthread
	echo -n '#define LIBPTHREAD_PATH "' > pthread_so_path.def
	ldd search_shlib_path | grep libpthread.so | awk '{printf $$3}' >> pthread_so_path.def
	echo '"' >> pthread_so_path.def
	rm search_shlib_path

#Object files
MAIN_OBJS = myth_log.o myth_sched.o myth_worker.o \
myth_malloc_wrapper.o myth_sync.o myth_init.o \
myth_misc.o myth_io.o myth_original_lib.o myth_tls.o myth_context.o

# Targets
libmyth.so: $(MAIN_OBJS) myth_if_native.o myth_if_pthread.o myth_constructor.o
	$(CC) $(CFLAGS) -o $@ $^ -ldl
libmyth-native.so: $(MAIN_OBJS) myth_if_native.o
	$(CC) $(CFLAGS) -o $@ $^ -ldl
libmyth-compat.so: $(MAIN_OBJS) myth_if_pthread.o myth_constructor.o
	$(CC) $(CFLAGS) -o $@ $^ -ldl

clean:
	rm -f *.d
	rm -f *.o
	rm -f *~
	rm -f pthread_so_path.def

distclean: clean
	rm -rf $(TARGET)

