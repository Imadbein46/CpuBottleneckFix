# Cross-compiles CpuBottleneckFix.asi for 32-bit and 64-bit games with mingw-w64 (win32 thread model, fully
# static: the .asi has no dependency besides Windows system DLLs).
#   make            -> build/x86/CpuBottleneckFix.asi and build/x64/CpuBottleneckFix.asi
#   make test       -> native unit tests (hotkey parser, FrameClock, bottleneck classifier)
VERSION  ?= 1.1.0
SRC      := src/plugin.cpp src/hotkey.cpp src/frame_clock.cpp src/config.cpp src/log.cpp src/cpu_tuner.cpp src/overlay.cpp \
            src/sources.cpp src/source_d3d8.cpp src/source_d3d9.cpp src/source_dxgi.cpp src/source_opengl.cpp src/source_vulkan.cpp
MH       := third_party/minhook
MHSRC    := $(MH)/src/buffer.c $(MH)/src/hook.c $(MH)/src/trampoline.c
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0601 -DCBF_VERSION=\"$(VERSION)\" -I$(MH)/include
CFLAGS   := -O2 -Wall -I$(MH)/include
LDFLAGS  := -shared -static -static-libgcc -static-libstdc++ -s -Wl,--kill-at -Wl,--enable-auto-import
LIBS     := -luser32 -lgdi32 -lkernel32 -ldxguid -luuid

all: build/x86/CpuBottleneckFix.asi build/x64/CpuBottleneckFix.asi

define BUILD
build/$(1)/CpuBottleneckFix.asi: $(SRC) $(MHSRC) $(MH)/src/hde/$(3).c src/*.h
	@mkdir -p build/$(1)/obj
	$(2)-gcc $(CFLAGS) -c $(MH)/src/buffer.c -o build/$(1)/obj/buffer.o
	$(2)-gcc $(CFLAGS) -c $(MH)/src/hook.c -o build/$(1)/obj/hook.o
	$(2)-gcc $(CFLAGS) -c $(MH)/src/trampoline.c -o build/$(1)/obj/trampoline.o
	$(2)-gcc $(CFLAGS) -c $(MH)/src/hde/$(3).c -o build/$(1)/obj/hde.o
	$(2)-g++ $(CXXFLAGS) $(SRC) build/$(1)/obj/*.o $(LDFLAGS) $(LIBS) -o $$@
endef
$(eval $(call BUILD,x86,i686-w64-mingw32,hde32))
$(eval $(call BUILD,x64,x86_64-w64-mingw32,hde64))

test:
	@mkdir -p build
	g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread tests/test_main.cpp src/hotkey.cpp src/frame_clock.cpp -o build/unit_tests
	./build/unit_tests

clean:
	rm -rf build
.PHONY: all test clean
