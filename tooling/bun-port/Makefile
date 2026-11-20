# PS5Term - Bun port: development receiver, ps5compat and feasibility probes. GPL-3.0-or-later.
# Built with the pinned PS5 payload SDK v0.45 (see tools/build-run-payload.sh).
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

SHSRV := ../../third_party/shsrv
OUT ?= ../../build/bun-port
CFLAGS := -Wall -Werror -O2 -fPIC -Ips5compat/include -include ps5compat_prelude.h
CXXFLAGS := -Wall -Werror -O2 -std=c++20

COMPAT_SRC := ps5compat/ps5compat.c ps5compat/spawn.c ps5compat/emutls.c ps5compat/fdpath.c ps5compat/syscall_errno.S \
              ../run-payload/ps5term_tty.c $(SHSRV)/elfldr.c $(SHSRV)/pt.c
COMPAT_OBJ := $(patsubst %,$(OUT)/obj/%.o,$(basename $(notdir $(COMPAT_SRC))))
COMPAT_LDFLAGS := -L$(OUT) -Wl,--whole-archive -lps5compat -Wl,--no-whole-archive \
                  $(shell cat ps5compat/ps5compat.ldflags) -lexecinfo -lkernel_sys
PROBES := probe_tls probe_exec probe_event probe_spawn probe_child probe_keys probe_fcntl probe_mmap probe_connect probe_umtx probe_openat probe_getdents probe_fdpath probe_faccessat

# FreeBSD base libraries that crates (libc, std) and build systems name with -l but whose
# functions a payload either gets from the SDK's libc/libkernel or must not use at all. Their
# archives are empty: any function really taken from them stays an undefined-symbol link error.
EMPTY_SYSLIBS := rt util kvm memstat procstat devstat gcc_s m pthread elf

all: $(OUT)/recv.elf $(OUT)/libps5compat.a $(OUT)/libexecinfo.a $(OUT)/probe_cxx.elf \
     $(PROBES:%=$(OUT)/%.elf) $(OUT)/launch.elf syslibs

syslibs: $(EMPTY_SYSLIBS:%=$(OUT)/syslibs/lib%.a)

$(OUT)/syslibs/lib%.a:
	mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@

$(OUT) $(OUT)/obj:
	mkdir -p $@

$(OUT)/recv.elf: recv/recv.c recv/sha256.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ recv/recv.c

$(OUT)/obj/%.o: ps5compat/%.c ps5compat/ps5compat.h | $(OUT)/obj
	$(CC) $(CFLAGS) -I$(SHSRV) -c -o $@ $<

$(OUT)/obj/%.o: ps5compat/%.S | $(OUT)/obj
	$(CC) -c -o $@ $<

$(OUT)/obj/%.o: ../run-payload/%.c | $(OUT)/obj
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/obj/%.o: $(SHSRV)/%.c | $(OUT)/obj
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/libps5compat.a: $(COMPAT_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

# For build systems that add -lexecinfo on FreeBSD (WebKit, Bun).
$(OUT)/libexecinfo.a: $(OUT)/obj/execinfo.o
	rm -f $@
	$(AR) rcs $@ $^

$(OUT)/probe_%.elf: probes/probe_%.c probes/probe.h $(OUT)/libps5compat.a $(OUT)/libexecinfo.a ps5compat/ps5compat.ldflags
	$(CC) $(CFLAGS) -Ips5compat -o $@ $< $(COMPAT_LDFLAGS)

$(OUT)/launch.elf: probes/launch.c probes/probe.h $(OUT)/libps5compat.a $(OUT)/libexecinfo.a ps5compat/ps5compat.ldflags
	$(CC) $(CFLAGS) -Ips5compat -o $@ $< $(COMPAT_LDFLAGS)

$(OUT)/probe_cxx.elf: probes/probe_cxx.cpp probes/probe.h $(OUT)/libps5compat.a $(OUT)/libexecinfo.a
	$(CXX) $(CXXFLAGS) -o $@ $< $(COMPAT_LDFLAGS)

.PHONY: all syslibs
