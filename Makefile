CC        := cc
CFLAGS    := -std=c11 -O2 -Wall -Wextra -Werror -Iruntime
OBJC      := clang
OBJCFLAGS := -O2 -Wall -Wextra -Werror -Iruntime

RT_OBJS       := runtime/build/rt_units.o runtime/build/rt_core.o runtime/build/rt_graph.o runtime/build/rt_caps.o runtime/build/rt_builtins.o
GUI_STUB_OBJ  := runtime/build/rt_gui_stub.o
GUI_COCOA_OBJ := runtime/build/rt_gui_cocoa.o
RT_LIB        := runtime/libthrumrt.a
SELFHOST      := selfhost/thrumc.thrum

.PHONY: all runtime thrumc_boot bootstrap tetris solitaire app native-test test clean

all: runtime thrumc tetris solitaire

runtime: $(RT_LIB) $(GUI_STUB_OBJ) $(GUI_COCOA_OBJ)

runtime/build/%.o: runtime/%.c runtime/thrum_rt.h
	@mkdir -p runtime/build
	$(CC) $(CFLAGS) -c $< -o $@

$(GUI_COCOA_OBJ): runtime/rt_gui_cocoa.m runtime/thrum_rt.h
	@mkdir -p runtime/build
	$(OBJC) $(OBJCFLAGS) -c $< -o $@

$(RT_LIB): $(RT_OBJS)
	rm -f $(RT_LIB)
	ar rcs $(RT_LIB) $(RT_OBJS)

thrumc: $(RT_LIB) $(GUI_STUB_OBJ) $(GUI_COCOA_OBJ) $(SELFHOST) selfhost/*.thrum
	THRUM_HOME=. python3 thrum.py run $(SELFHOST) -- emit-c $(SELFHOST) -o runtime/build/s1.c
	$(CC) $(CFLAGS) runtime/build/s1.c $(RT_LIB) $(GUI_STUB_OBJ) -lm -o runtime/build/s1
	THRUM_HOME=. runtime/build/s1 emit-c $(SELFHOST) -o runtime/build/s2.c
	$(CC) $(CFLAGS) runtime/build/s2.c $(RT_LIB) $(GUI_STUB_OBJ) -lm -o thrumc
	@rm -f runtime/build/s1.c runtime/build/s1 runtime/build/s2.c

bootstrap: thrumc
	THRUM_HOME=. ./thrumc emit-c $(SELFHOST) -o runtime/build/b2.c
	$(CC) $(CFLAGS) runtime/build/b2.c $(RT_LIB) $(GUI_STUB_OBJ) -lm -o runtime/build/b2
	THRUM_HOME=. runtime/build/b2 emit-c $(SELFHOST) -o runtime/build/b3.c
	diff -u runtime/build/b2.c runtime/build/b3.c
	@rm -f runtime/build/b2.c runtime/build/b2 runtime/build/b3.c
	@echo "Bootstrap fixed point verified."

tetris: $(RT_LIB) $(GUI_STUB_OBJ) thrumc examples/tetris.thrum
	THRUM_HOME=. ./thrumc build examples/tetris.thrum -o tetris

solitaire: $(RT_LIB) $(GUI_COCOA_OBJ) thrumc examples/solitaire.thrum
	THRUM_HOME=. ./thrumc build examples/solitaire.thrum -o solitaire

app: solitaire assets/thrum_logo.png
	@rm -rf ThrumSolitaire.app runtime/build/AppIcon.iconset
	@mkdir -p ThrumSolitaire.app/Contents/MacOS ThrumSolitaire.app/Contents/Resources runtime/build/AppIcon.iconset
	@cp solitaire ThrumSolitaire.app/Contents/MacOS/solitaire
	@chmod +x ThrumSolitaire.app/Contents/MacOS/solitaire
	@sips -z 16 16     assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_16x16.png >/dev/null
	@sips -z 32 32     assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_16x16@2x.png >/dev/null
	@sips -z 32 32     assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_32x32.png >/dev/null
	@sips -z 64 64     assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_32x32@2x.png >/dev/null
	@sips -z 128 128   assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_128x128.png >/dev/null
	@sips -z 256 256   assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_128x128@2x.png >/dev/null
	@sips -z 256 256   assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_256x256.png >/dev/null
	@sips -z 512 512   assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_256x256@2x.png >/dev/null
	@sips -z 512 512   assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_512x512.png >/dev/null
	@sips -z 1024 1024 assets/thrum_logo.png --out runtime/build/AppIcon.iconset/icon_512x512@2x.png >/dev/null
	@iconutil -c icns runtime/build/AppIcon.iconset -o ThrumSolitaire.app/Contents/Resources/AppIcon.icns
	@rm -rf runtime/build/AppIcon.iconset
	@printf '%s\n' \
		'<?xml version="1.0" encoding="UTF-8"?>' \
		'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
		'<plist version="1.0">' \
		'<dict>' \
		'  <key>CFBundleDevelopmentRegion</key><string>en</string>' \
		'  <key>CFBundleExecutable</key><string>solitaire</string>' \
		'  <key>CFBundleIconFile</key><string>AppIcon</string>' \
		'  <key>CFBundleIdentifier</key><string>dev.thrum.solitaire</string>' \
		'  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>' \
		'  <key>CFBundleName</key><string>Thrum Solitaire</string>' \
		'  <key>CFBundlePackageType</key><string>APPL</string>' \
		'  <key>CFBundleShortVersionString</key><string>0.2.0</string>' \
		'  <key>NSHighResolutionCapable</key><true/>' \
		'</dict>' \
		'</plist>' > ThrumSolitaire.app/Contents/Info.plist
	@echo "Built ThrumSolitaire.app cleanly."

native-test: all
	python3 tests/run_native_tests.py

test: native-test
	python3 tests/run_tests.py
	python3 thrum.py test
	python3 thrum.py test tests/stdlib_test.thrum

clean:
	rm -rf runtime/build $(RT_LIB) thrumc tetris solitaire ThrumSolitaire.app
