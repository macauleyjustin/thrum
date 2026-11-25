CC        := cc
CFLAGS    := -std=c11 -O2 -Wall -Wextra -Werror -Iruntime
OBJC      := clang
OBJCFLAGS := -O2 -Wall -Wextra -Werror -Iruntime

RT_OBJS       := runtime/build/rt_units.o runtime/build/rt_core.o runtime/build/rt_graph.o runtime/build/rt_caps.o runtime/build/rt_builtins.o
GUI_STUB_OBJ  := runtime/build/rt_gui_stub.o
GUI_COCOA_OBJ := runtime/build/rt_gui_cocoa.o
NET_STUB_OBJ  := runtime/build/rt_net_stub.o
NET_APPLE_OBJ := runtime/build/rt_net_apple.o
RT_LIB        := runtime/libthrumrt.a
SELFHOST      := selfhost/thrumc.thrum

.PHONY: all runtime bootstrap tetris native-test test clean

all: runtime thrumc tetris

runtime: $(RT_LIB) $(GUI_STUB_OBJ) $(GUI_COCOA_OBJ) $(NET_STUB_OBJ) $(NET_APPLE_OBJ)

runtime/build/%.o: runtime/%.c runtime/thrum_rt.h
	@mkdir -p runtime/build
	$(CC) $(CFLAGS) -c $< -o $@

$(GUI_COCOA_OBJ): runtime/rt_gui_cocoa.m runtime/thrum_rt.h
	@mkdir -p runtime/build
	$(OBJC) $(OBJCFLAGS) -c $< -o $@

$(NET_APPLE_OBJ): runtime/rt_net_apple.m runtime/thrum_rt.h
	@mkdir -p runtime/build
	$(OBJC) $(OBJCFLAGS) -c $< -o $@

$(RT_LIB): $(RT_OBJS)
	rm -f $(RT_LIB)
	ar rcs $(RT_LIB) $(RT_OBJS)

thrumc: $(RT_LIB) $(GUI_STUB_OBJ) $(GUI_COCOA_OBJ) $(NET_STUB_OBJ) $(NET_APPLE_OBJ) $(SELFHOST) selfhost/*.thrum
	THRUM_HOME=. python3 thrum.py run $(SELFHOST) -- emit-c $(SELFHOST) -o runtime/build/s1.c
	$(CC) $(CFLAGS) runtime/build/s1.c $(RT_LIB) $(GUI_STUB_OBJ) $(NET_STUB_OBJ) -lm -o runtime/build/s1
	THRUM_HOME=. runtime/build/s1 emit-c $(SELFHOST) -o runtime/build/s2.c
	$(CC) $(CFLAGS) runtime/build/s2.c $(RT_LIB) $(GUI_STUB_OBJ) $(NET_STUB_OBJ) -lm -o thrumc
	@rm -f runtime/build/s1.c runtime/build/s1 runtime/build/s2.c

bootstrap: thrumc
	THRUM_HOME=. ./thrumc emit-c $(SELFHOST) -o runtime/build/b2.c
	$(CC) $(CFLAGS) runtime/build/b2.c $(RT_LIB) $(GUI_STUB_OBJ) $(NET_STUB_OBJ) -lm -o runtime/build/b2
	THRUM_HOME=. runtime/build/b2 emit-c $(SELFHOST) -o runtime/build/b3.c
	diff -u runtime/build/b2.c runtime/build/b3.c
	@rm -f runtime/build/b2.c runtime/build/b2 runtime/build/b3.c
	@echo "Bootstrap fixed point verified."

tetris: $(RT_LIB) $(GUI_STUB_OBJ) $(NET_STUB_OBJ) thrumc examples/tetris.thrum
	THRUM_HOME=. ./thrumc build examples/tetris.thrum -o tetris

native-test: all
	python3 tests/run_native_tests.py

test: native-test
	python3 tests/run_tests.py
	python3 thrum.py test
	python3 thrum.py test tests/stdlib_test.thrum

clean:
	rm -rf runtime/build $(RT_LIB) thrumc tetris
