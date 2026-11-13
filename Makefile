CC = clang
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
OBJCFLAGS = -fobjc-arc -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
B = build

SLIRP_CFLAGS = $(shell PATH=/opt/homebrew/bin:$$PATH pkg-config --cflags slirp)
SLIRP_LIBS = $(shell PATH=/opt/homebrew/bin:$$PATH pkg-config --libs slirp)

CORE = $(B)/classify.o $(B)/operands.o $(B)/translate.o $(B)/dispatch.o $(B)/mmu.o $(B)/memop.o \
       $(B)/exception.o $(B)/sysreg.o $(B)/machine.o
HOST = $(B)/jit_region.o $(B)/enter.o $(B)/disk.o
ENGINE = $(CORE) $(HOST)

DEV = $(B)/acpi.o $(B)/fb_display.o $(B)/fb_metal.o $(B)/fdt.o $(B)/fw_cfg.o $(B)/gicv3.o $(B)/hda.o \
      $(B)/nvme.o $(B)/pcie.o $(B)/pflash.o $(B)/pl011.o $(B)/pl031.o $(B)/pl061.o $(B)/timer.o \
      $(B)/virtio.o $(B)/virtio_blk.o $(B)/virtio_gpu.o $(B)/virtio_input.o $(B)/virtio_net.o $(B)/virtio_snd.o \
      $(B)/winvirt.o $(B)/xhci.o

VM_HOST = $(B)/winvm.o $(B)/net_slirp.o $(B)/share.o $(B)/audio_mac.o $(B)/hvsparse.o $(B)/perf.o

MAC_FRAMEWORKS = -framework Cocoa -framework Foundation -framework QuartzCore -framework Metal \
                 -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers \
                 -framework AudioToolbox

CORE_TESTS = $(B)/test_classify $(B)/test_jit_region $(B)/test_operands $(B)/test_jit $(B)/test_mem \
             $(B)/test_mmu $(B)/test_exc $(B)/test_virtio_gpu $(B)/test_xhci_touch $(B)/test_its $(B)/test_nvme

all: $(CORE_TESTS) $(B)/hvsparse $(B)/winvm

# mmu.c: no SIMD (hv_tlb_refill runs with the guest's SIMD registers live).
$(B)/mmu.o: core/mmu.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -mgeneral-regs-only -c $< -o $@
$(B)/net_slirp.o: host/net_slirp.c host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) $(SLIRP_CFLAGS) -c $< -o $@
$(B)/fb_metal.o: devices/fb_metal.m devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(OBJCFLAGS) -c $< -o $@
$(B)/main_mac.o: host/main_mac.m host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(OBJCFLAGS) -c $< -o $@
$(B)/%.o: core/%.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.c host/*.h core/*.h devices/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.S | $(B)
	$(CC) -c $< -o $@
$(B)/%.o: devices/%.c devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/winvm: $(B)/main_mac.o $(ENGINE) $(DEV) $(VM_HOST)
	$(CC) $(B)/main_mac.o $(ENGINE) $(DEV) $(VM_HOST) $(SLIRP_LIBS) $(MAC_FRAMEWORKS) -o $@

$(B)/test_classify: tests/test_classify.c tests/classify_cases.s $(B)/classify.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_jit_region: tests/test_jit_region.c $(B)/jit_region.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_operands: tests/test_operands.c tests/operand_cases.s $(B)/operands.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_mmu: tests/test_mmu.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_mmu.c $(ENGINE) -o $@

$(B)/test_virtio_gpu: tests/test_virtio_gpu.c $(ENGINE) $(B)/gicv3.o $(B)/pcie.o $(B)/virtio.o $(B)/virtio_gpu.o $(B)/fb_display.o $(B)/fw_cfg.o
	$(CC) $(CFLAGS) tests/test_virtio_gpu.c $(ENGINE) $(B)/gicv3.o $(B)/pcie.o $(B)/virtio.o $(B)/virtio_gpu.o $(B)/fb_display.o $(B)/fw_cfg.o -o $@
$(B)/test_xhci_touch: tests/test_xhci_touch.c $(ENGINE) $(B)/xhci.o $(B)/pcie.o $(B)/gicv3.o
	$(CC) $(CFLAGS) tests/test_xhci_touch.c $(ENGINE) $(B)/xhci.o $(B)/pcie.o $(B)/gicv3.o -o $@
$(B)/test_its: tests/test_its.c $(ENGINE) $(B)/gicv3.o
	$(CC) $(CFLAGS) tests/test_its.c $(ENGINE) $(B)/gicv3.o -o $@
$(B)/test_nvme: tests/test_nvme.c $(ENGINE) $(B)/nvme.o $(B)/pcie.o $(B)/gicv3.o
	$(CC) $(CFLAGS) tests/test_nvme.c $(ENGINE) $(B)/nvme.o $(B)/pcie.o $(B)/gicv3.o -o $@

$(B)/hvsparse: tools/hvsparse_main.c host/hvsparse.c host/hvsparse.h | $(B)
	$(CC) $(CFLAGS) tools/hvsparse_main.c host/hvsparse.c -o $@

$(B)/test_%: tests/test_%.c tests/%_programs.S tests/harness.S tests/harness.h $(ENGINE)
	$(CC) $(CFLAGS) -Itests tests/test_$*.c tests/$*_programs.S tests/harness.S $(ENGINE) -o $@

test-core: $(CORE_TESTS)
	@ulimit -s 65520; for t in $(CORE_TESTS); do echo "== $$t"; $$t || exit 1; done

$(B):
	mkdir -p $(B)

# ---- iOS static library target: build/ios/libwinvm.a ----
IOS_SDK = $(shell xcrun --sdk iphoneos --show-sdk-path)
IOS_CC = xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot $(IOS_SDK)
IOS_SLIRP_INC ?= ios/deps/include/slirp
IOS_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices -I$(IOS_SLIRP_INC)
IOS_OBJCFLAGS = -fobjc-arc -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
IB = build/ios

IOS_C_SRC = $(wildcard core/*.c) \
            host/jit_region.c host/disk.c host/winvm.c host/net_slirp.c host/share.c \
            host/audio_mac.c host/hvsparse.c host/prof_ios.c host/perf.c \
            $(wildcard devices/*.c)
IOS_OBJ = $(patsubst %.c,$(IB)/%.o,$(IOS_C_SRC)) $(IB)/host/enter.o $(IB)/devices/fb_metal.o

$(IB)/core/mmu.o: core/mmu.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -mgeneral-regs-only -c $< -o $@
$(IB)/%.o: %.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -c $< -o $@
$(IB)/devices/fb_metal.o: devices/fb_metal.m devices/*.h core/*.h host/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_OBJCFLAGS) -c $< -o $@
$(IB)/host/enter.o: host/enter.S
	@mkdir -p $(dir $@)
	$(IOS_CC) -c $< -o $@
$(IB)/libwinvm.a: $(IOS_OBJ)
	rm -f $@
	xcrun --sdk iphoneos libtool -static -o $@ $(IOS_OBJ)

ios-lib: $(IB)/libwinvm.a

# ---- freestanding Windows ARM64 guest tools: build/*.exe ----
# Import libraries are made from the .def files generated below, one per DLL, listing every function
# the tools import (a superset is harmless: only the functions a tool calls end up in its import table).
WIN_LLVM = /opt/homebrew/opt/llvm/bin
WIN_CC = $(WIN_LLVM)/clang --target=aarch64-pc-windows-msvc -ffreestanding -fno-builtin \
         -fno-stack-protector -nostdlib -fuse-ld=lld -O2 -Wall -Wextra
WL = $(B)/winlibs

KERNEL32_IMPORTS = CloseHandle CreateFileW CreateMutexW CreateProcessW CreateThread \
                   ExitProcess GetCommandLineW GetCurrentProcess GetFileAttributesW GetLastError \
                   GetModuleFileNameW GetProcessHeap GetStdHandle GetTempPathW GetTickCount64 HeapAlloc \
                   HeapFree MultiByteToWideChar Sleep WaitForMultipleObjects WideCharToMultiByte WriteFile
ADVAPI32_IMPORTS = AdjustTokenPrivileges LookupPrivilegeValueW OpenProcessToken RegCloseKey RegCreateKeyExW \
                   RegOpenKeyExW RegQueryValueExW RegSetValueExW
WS2_32_IMPORTS = WSAStartup accept bind closesocket connect listen recv send setsockopt shutdown socket
SETUPAPI_IMPORTS = SetupDiDestroyDeviceInfoList SetupDiEnumDeviceInterfaces SetupDiGetClassDevsW \
                   SetupDiGetDeviceInterfaceDetailW

empty :=
space := $(empty) $(empty)
define WINLIB
$(WL)/$(1).def: Makefile | $(WL)
	printf 'LIBRARY $(1).dll\nEXPORTS\n$(subst $(space),\n,$(2))\n' > $$@
$(WL)/$(1).lib: $(WL)/$(1).def
	$(WIN_LLVM)/llvm-dlltool -m arm64 -d $$< -l $$@
endef
$(eval $(call WINLIB,kernel32,$(KERNEL32_IMPORTS)))
$(eval $(call WINLIB,advapi32,$(ADVAPI32_IMPORTS)))
$(eval $(call WINLIB,ws2_32,$(WS2_32_IMPORTS)))
$(eval $(call WINLIB,setupapi,$(SETUPAPI_IMPORTS)))

$(WL):
	mkdir -p $(WL)

$(B)/opt_hive_arm64.exe: tools/opt_hive_arm64.c $(WL)/kernel32.lib $(WL)/advapi32.lib
	$(WIN_CC) -Wl,/entry:mainCRTStartup -Wl,/subsystem:console $< $(WL)/kernel32.lib $(WL)/advapi32.lib -o $@
$(B)/winios_browser_arm64.exe: tools/winios_browser_arm64.c $(WL)/kernel32.lib $(WL)/ws2_32.lib
	$(WIN_CC) -Wl,/entry:WinMainCRTStartup -Wl,/subsystem:windows $< $(WL)/kernel32.lib $(WL)/ws2_32.lib -o $@
$(B)/winios_agent_arm64.exe: tools/winios_agent_arm64.c $(WL)/kernel32.lib $(WL)/advapi32.lib $(WL)/ws2_32.lib $(WL)/setupapi.lib
	$(WIN_CC) -Wl,/entry:WinMainCRTStartup -Wl,/subsystem:windows $< $(WL)/kernel32.lib $(WL)/advapi32.lib \
	    $(WL)/ws2_32.lib $(WL)/setupapi.lib -o $@

guest-tools: $(B)/winios_agent_arm64.exe $(B)/opt_hive_arm64.exe $(B)/winios_browser_arm64.exe

clean:
	rm -rf $(B)

.PHONY: all test-core ios-lib guest-tools clean
