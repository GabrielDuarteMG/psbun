# Native Bun port: current validation

The narrative is [docs/BUN_PORT.md](../../docs/BUN_PORT.md) (psbun, Bun station 5 port). This file is the console log.

## Console gate stopped (2026-11-20)

The Bun executable linked and stripped successfully (120441032 bytes).
Uploaded SHA-256: `107438dacad38c95d12d328cb5024bf27e932a5a1dad4787fbd197f71a66133c`.
The corresponding unstripped `bun-profile` passed `audit_syscalls.py`: 39 syscall sites, zero failures.
The export check passed with 818 exported symbols. Upstream FreeBSD checks still report differences in SDK shared libraries, exception personality, TLS initialization and payload segment layout; these checks have not been adapted or waived.

The first `--version` execution through the development launcher timed out after 60 seconds without output. The harness recorded launcher PID 382 and child PID 383. At timeout its process snapshot showed launcher PID 382, which it killed; the captured output did not establish the child's exit status. Execution stopped until the user authorized renewed diagnosis.

Captured relevant output:

```text
[launch] PID 382
[launch] listening on 9301
[launch] child PID 383
TIMEOUT launch after 60s
ps: 382         1       89       89        0      SLEEP   0000                3.8 /   16.6  payload.elf
killed own pid 382
--- program output so far ---
```

The uploaded test executable remains at `/data/ps5term-dev/bun.elf`; cleanup was not attempted after the stop. Bun is not console-validated, and addon/integration gates must not proceed.

## Authorized diagnostic rerun

A read-only process snapshot after the first stop showed all three required daemons and no PID 382 or 383. The user then authorized diagnosis. A temporary host-side wrapper captured klog before the harness's `[launch]` filter, without changing the binary, timeout or PID cleanup. The wrapper was restored afterward.

The single rerun recorded launcher PID 415 and Bun child PID 416. The kernel reported:

```text
# signal: 11 (SIGSEGV)
# proc ID: 416
# proc name: /data/ps5term-dev/bun.elf
# reason: page fault (user read data, page not present)
# fault address: 0000000000000028
# r14: 0000000000000000
# rip: 00000002020ff540
# backtrace:
# 000000020244bfe4
# 0000000202445cc7
# 0000000202445a4c
```

The launcher still timed out at 60 seconds with empty stdout; the harness killed its own PID 415. No further console requests were made after this timeout.

Local symbolization with inferred load base `0x200004000` resolves the fault to `_mi_auto_process_init`, `vendor/mimalloc/src/init.c:627`, and the stack to `payload_init`, `__crt_start`, `_start`. This base is supported by matching all stack addresses and the fault instruction: ELF address `0x20fb540` is `cmpq $0, 0x28(%r14)`, consistent with the recorded zero register and fault address. The source dereferences `subproc->theap_meta` after `_mi_theap_subproc(theap)` returns null. Why allocator initialization left that pointer null remains unresolved; this is not evidence of a JIT or terminal failure.

The existing launcher-only log filter hid the fatal-signal report. No runtime fix or passing Bun gate is claimed.

## mimalloc TLS fix (2026-11-20)

Cause: the PS5 target only has emulated TLS, so mimalloc's `__thread` default theap was still the empty sentinel after `mi_process_init` (`theap->heap == NULL`, read at `+0x28`). `scripts/build/deps/mimalloc.ts` now builds mimalloc with `MI_TLS_MODEL_PTHREADS=1` (no `-ftls-model`) for `x86_64-sie-ps5`. Rebuilt binary: `_mi_auto_process_init` no longer touches `__emutls_v.__mi_theap_default`; `audit_syscalls.py`: 39 sites, 0 failures; upload SHA-256 `07317f7b080afa92d83e8b14d9bf9809142d4cdbc198e692f1288db315b949ae`.

Rerun of `--version` (launcher PID 435, child PID 436): startup now passes the allocator and reaches Bun's crash handler, which printed its banner (`Bun v1.4.4 (38afee6c8) FreeBSD x64`, `CPU: sse42 popcnt avx avx2`) and then produced nothing more; timeout at 60 s, launcher 435 killed. No kernel fatal-signal report for 436 was captured. Execution stopped again per the safety gate.

## Runtime fixes after the first start (2026-11-21)

Gate set passing on the console: `--version` → `1.4.4`; `-e 'console.log(process.platform, process.arch, 1+1)'` → `freebsd x64 2`; `Bun.$\`echo hi\`` → `hi`; `Bun.spawn([bun.elf, "-e", "console.log(7)"])` → `7 0`; `fetch("https://google.com/")` and `fetch("https://example.com/")` → 200; `tls.connect` to google.com → TLSv1.3, authorized.

Each fix is limited to the PS5 build (`x86_64-sie-ps5`, `__PROSPERO__`, `libc_unstable_freebsd_version = "11"`) or lives in ps5compat:

| Symptom | Cause | Fix |
| --- | --- | --- |
| Crash in mimalloc on the first allocation of a second thread | `%fs:0` is not a unique per-thread pointer on the PS5 | `vendor/mimalloc/include/mimalloc/prim-tls.h`: `MI_PRIM_THREAD_ID()` is `pthread_self()` under `__PROSPERO__` |
| `Bun.$` fails | The kernel refuses `F_DUPFD_CLOEXEC` with `EOPNOTSUPP` on every descriptor type (`probe_fcntl`) | `__wrap_fcntl` composes `F_DUPFD` + `F_SETFD FD_CLOEXEC` |
| `dns.lookup` fails with `EAI_SERVICE` | The console's `getaddrinfo` refuses a numeric service without a socket type | `__wrap_getaddrinfo` resolves without the service and stores the port |
| `DNS_ECONNREFUSED` | c-ares finds no `/etc/resolv.conf` and asks 127.0.0.1 | `src/dns/lib.rs`: the default backend is `System` (the console's own resolver; its DNS server is not changed) |
| Any failed `openat`, `fstatat`, `unlinkat`, `mkdirat`, `renameat`, … returns a "success"; HTTPS hung in `read(2, …)` | The SDK's `libc.a` (`syscalls.o`) defines 33 calls as `syscall; ret` with no carry check, so the kernel's error number comes back as the result: `openat("/etc/ssl/cert.pem")` returned 2 (ENOENT), Bun read that "descriptor" (stderr, the host socket) and blocked (`probe_openat`) | `ps5compat/syscall_errno.S`: `__wrap_NAME` calls the SDK stub and, on carry (`ret` keeps the flags), sets `errno` and returns -1. No new system call instruction; `syscall`, `ptrace` and `posix_fadvise` (which returns its error by design) are left alone |
| `bun install` fails with `failed to resolve cache dir … EINVAL` | Bun turns descriptors back into paths with FreeBSD 13's `fcntl(F_KINFO)`; the PS5 kernel refuses it (EINVAL) and also lacks FreeBSD 11's `sysctl kern.proc.filedesc` (ENOENT) (`probe_fdpath`) | `ps5compat/fdpath.c` remembers the path given to `open`/`openat`/`dup`/`dup2`/`fcntl(F_DUPFD*)` and forgets it in `close`; `ps5compat_fd_path` returns `realpath` of it only while `stat` and `fstat` agree on device and inode (otherwise ENOENT). `bun_sys::get_fd_path` and `bun_core::fd_path_raw` call it on the PS5 |
| `fs.copyFileSync` fails with `ENOSYS` | The kernel has no `copy_file_range`, and Bun's FreeBSD code falls back to read/write only on EXDEV/EINVAL/EOPNOTSUPP/EBADF | ps5compat's `copy_file_range` is a read/write loop with the system call's offset and return semantics (what FreeBSD's kernel does for file systems without a native copy) |
| `bun install`: `EOPNOTSUPP: failed copying files from cache` | `/data` supports neither hard links nor symbolic links (`link` and `symlink` both fail with EOPNOTSUPP) | `src/install/PackageInstall.rs`: on the PS5 an `EOPNOTSUPP` from `linkat` is treated like `EXDEV`, so Bun switches to copying |
| `bun install --force`: `the downloaded package was not found in the cache` | The kernel answers `faccessat` and `readlinkat` with ENOSYS, `utimensat` with EACCES and `futimens` with ESRCH; `eaccess` is another bare SDK stub and gives ENOSYS (`probe_faccessat`). Payloads run with real uid 1 and effective uid 0 | `ps5compat/fdpath.c` implements them without ever making those calls: `faccessat` `F_OK` with `fstatat`, other modes with `access` on the directory descriptor's path (real ids) or, for `AT_EACCESS` and `eaccess`, the kernel's permission rule with the effective ids; `readlinkat` with `readlink`; `utimensat`/`futimens` with `utimes`/`lutimes`/`futimes` (microsecond precision, `UTIME_NOW`/`UTIME_OMIT` honoured). `/data` (nullfs) ignores access times even for the kernel's own `utimes` |
| `bun add semver`: `failed to link semver: EOPNOTSUPP` | `node_modules/.bin` entries are symbolic links, which `/data` cannot hold | `src/install/bin.rs` (`symlink_or_shim`, `readlink_or_shim`) and `src/install/prune.rs`: on the PS5, an `EOPNOTSUPP` from `symlink` writes a bin shim (`ps5compat/binshim.c`), which Bun reads back as a link. `ps5compat/spawn.c` runs the shim's target in `bun.elf` |
| `readdir` of anything under `/data` fails with `EINVAL` | `/data`'s file system refuses `getdents` buffers smaller than its 64 KiB `st_blksize`; `/` accepts 512 bytes (`probe_getdents`) | `src/sys/lib.rs` and `src/runtime/node/dir_iterator.rs`: 128 KiB heap buffer on the PS5 (8 KiB inline elsewhere) |

`__wrap_mmap` logs every failed `mmap` to klog (`ps5compat: mmap(...) pid N: errno E`). It was added while chasing an intermittent libpas `PAS_ASSERT` (`pas_compact_heap_reservation_try_allocate`) in nested `Bun.spawn`; 13 of 14 runs passed and no failure has been logged since. `probe_mmap` shows 1 GiB reservable and 512 MiB touchable, so it is not a memory budget.

The crash handler prints `PS5 crash:`, `rip`, `rsp` and 600 stack words before its metadata (which hangs after `Args:` when stderr is a socket). The PS5 `mcontext` has six more words than FreeBSD's before `mc_trapno`, so `rip` is ucontext word 28 and `rsp` word 31; `mc_rip`/`mc_rbp` from the FreeBSD layout are wrong there. To see where a hung process waits, send it `kill -s SEGV <pid>` through shsrv and symbolize the stack words with `llvm-addr2line -e bun-profile <address - 0x200004000>`.

Test harness: `ps5_bun_console.run` stores arguments one per line, so it now refuses arguments containing a newline (multi-line `-e` scripts were silently cut after their first line and looked like hangs). The build does not track `libps5compat.a`: after changing only ps5compat sources, delete `build/ps5/bun` and `bun-profile` to force a relink.

Out of scope by the user's decision: the oh-my-pi (`omp`) addon, its global install tree and the `omp` command.

## Link and compatibility changes

The Sony clang driver forces unified full LTO, incompatible with the Rust ThinLTO inputs. The link uses the FreeBSD ELF driver with explicit SDK startup objects, SDK linker script, PIE and 16 KiB page alignment. Compilation remains targeted to the PS5 SDK. The generic Bun ELF export script avoids the FreeBSD CRT-only `environ` and `__progname` exports.

Socket-backed terminals have no device pathname (`ttyname_r` reports `ENOTTY`, or the descriptor error); PTY creation reports `ENOSYS`. `initgroups` reads `/etc/group` before calling the SDK `setgroups`; a missing/unreadable database fails without changing groups. This API has compiled but has not been exercised on the console.

SDK syscall constants 27 (`recvmsg`), 28 (`sendmsg`), 93 (`select`) and 97 (`socket`) were added to the audit allowlist after checking `target/include/sys/syscall.h` and references in SDK libc routines.

Source patches and the reproducible Bun build stage remain unfinished; current Bun changes are in `.deps/bun-port/bun` and the build environment is in `build/bun-port/bunenv.sh`.
