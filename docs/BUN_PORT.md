# psbun

Bun station 5 port. A native [Bun](https://bun.sh) payload for a jailbroken PlayStation 5, built in this fork of [PS5Term](https://github.com/phantomptr/ps5term).

<p align="center">
  <img src="images/ps5-bun.jpg" alt="psbun, Bun station 5 port" width="360" />
</p>

The terminal app is still PS5Term. psbun is the name of this port: Bun running as a PS5 payload on the console's own FreeBSD, not inside PS5Term's RISC-V Linux guest. The dated console log is [tooling/bun-port/README.md](../tooling/bun-port/README.md). This page is the narrative. It does not repeat every PID.

Bun here is 1.4.4 (`38afee6c84fcf48c0edb2466aa4b27d287a59c30` in [tooling/bun-port/sources.json](../tooling/bun-port/sources.json)). The target is the FreeBSD 11 ABI of ps5-payload-sdk v0.45 (`x86_64-ps5-freebsd` for Rust, `x86_64-sie-ps5` / `__PROSPERO__` for C and C++).

## Where the shape comes from

A payload this large is launched the way [ProsperoWeb](https://github.com/kvaps/ProsperoWeb) launches its browser: the open ps5-payload-sdk, an ELF loader on port 9021, 16 KiB pages, a static link. The WebKit that is compiled is Bun's fork (`samm-git/WebKit`, pinned in `sources.json`), not ProsperoWeb's tree.

ProsperoWeb turns the JavaScript JIT on. psbun compiles the JIT tiers, because Bun's WebKit fork does not build with `ENABLE_JIT=OFF` (DOMJIT still needs the DFG headers), and then forces the LLInt interpreter. [tooling/bun-port/patches/webkit-ps5-nojit.patch](../tooling/bun-port/patches/webkit-ps5-nojit.patch) makes `Options::useJIT()` false on a PS5 payload, and the launcher sets `BUN_JSC_useJIT=0`. No executable memory is allocated for JIT code.

The checked-in patches and `tools/build-bun-port.sh` do not yet reproduce the binary that ran on the console. Bun source changes live in `.deps/bun-port/bun`, and the build environment in `build/bun-port/bunenv.sh`. Those directories are not part of the repository.

## What has run on a console

Every result below is from the development launcher, against `/data/ps5term-dev/bun.elf`. The `bun` command inside a PS5Term tab has not been the thing under test.

The first `--version` never printed. The kernel log, hidden at first by the launcher's `[launch]` filter, was a `SIGSEGV` in `_mi_auto_process_init` (`vendor/mimalloc/src/init.c`). The PS5 loader does not set up ELF TLS, so mimalloc's `__thread` default heap was still the empty sentinel and the faulting read was at offset `0x28`. Building mimalloc with `MI_TLS_MODEL_PTHREADS=1` got past that. A second thread then crashed on the first allocation because `%fs:0` is not a unique per-thread pointer here; `MI_PRIM_THREAD_ID()` is `pthread_self()` under `__PROSPERO__`.

After those fixes, this set passes:

| Command | Result |
| --- | --- |
| `--version` | `1.4.4` |
| `-e 'console.log(process.platform, process.arch, 1+1)'` | `freebsd x64 2` |
| `` Bun.$`echo hi` `` | `hi` |
| `Bun.spawn` of `bun.elf -e 'console.log(7)'` | `7`, exit 0 |
| `fetch("https://google.com/")` and `fetch("https://example.com/")` | 200 |
| `tls.connect` to google.com | TLSv1.3 |
| `bun install`, `bun install --force`, `bun add semver` | packages install; `require` loads them |
| `fs.copyFileSync`, `fs.utimesSync`, `fs.accessSync`, `existsSync` | correct results |

The syscall audit reports zero failures. `elfldr`, `shsrv` and `klogsrv` were still running after the install runs. The earlier binaries were counted at 39 syscall sites; the install build's diary does not repeat a site count, so this page does not reuse that number.

The first `bun install` wrote its cache under `/data/ps5term/bun`, one of the install directories Bun already allows, because `/data/ps5term` did not exist yet. That cache, the test project and the test cache were deleted afterwards. Later runs pass `--cache-dir /data/ps5term-dev/cache`. That development cache is not the copy the app will keep at `/data/ps5term/bun`.

## Why `bun install` needed a layer of its own

Nothing already published solves this. Bun asks the kernel for a function the PS5 kernel does not have, and reading kernel memory to recover it is out of the question. Each failure was measured with a probe on the console (`probe_fdpath`, `probe_faccessat`) before the workaround, and the probes pass with the layer in place.

**The path of an open file.** Bun recovers it with `fcntl(F_KINFO)`, which exists from FreeBSD 13. On FreeBSD 11 the documented way is `sysctl kern.proc.filedesc`. On the console the first returns `EINVAL` and the second `ENOENT`: Sony removed the sysctl node. [tooling/bun-port/ps5compat/fdpath.c](../tooling/bun-port/ps5compat/fdpath.c) remembers the path passed to `open`, `openat`, `dup`, `dup2` and `fcntl(F_DUPFD*)`, forgets it in `close`, and `ps5compat_fd_path` returns `realpath` of that path only while `stat` and `fstat` agree on device and inode. `bun_sys::get_fd_path` and `bun_core::fd_path_raw` call it on the PS5. Without it, `bun install` stopped at `failed to resolve cache dir … EINVAL`.

**`/data` has no hard links and no symbolic links.** `link` and `symlink` both return `EOPNOTSUPP`. In `src/install/PackageInstall.rs`, an `EOPNOTSUPP` from `linkat` is treated like `EXDEV`, so Bun copies the package. That is the path Bun already takes on other file systems that cannot link.

**`fs.copyFileSync` returned `ENOSYS`.** The kernel has no `copy_file_range`, and Bun's FreeBSD fallback to read/write only runs for `EXDEV`, `EINVAL`, `EOPNOTSUPP` and `EBADF`. ps5compat's `copy_file_range` is a read/write loop with the system call's offset and return rules, which is what FreeBSD itself does for a file system with no native copy.

**`faccessat` and `readlinkat` always return `ENOSYS`.** `utimensat` returns `EACCES` and `futimens` returns `ESRCH` ("process not found"), which is a sign the call may be landing on a different Sony function. `eaccess` is another bare SDK stub and also returns `ENOSYS`. Payloads run with real uid 1 and effective uid 0. The layer never issues those calls. `faccessat` with `F_OK` uses `fstatat`; other modes use `access` on the directory descriptor's path (real ids) or, for `AT_EACCESS` and `eaccess`, the kernel's permission rule with the effective ids. `readlinkat` uses `readlink`. `utimensat` and `futimens` use `utimes`, `lutimes` and `futimes` (microsecond precision, `UTIME_NOW` and `UTIME_OMIT` honoured). `/data` is nullfs and ignores access times even for the kernel's own `utimes`. This is what let `bun install --force` get past `the downloaded package was not found in the cache`.

**Package executables need a symlink.** `node_modules/.bin` entries are symbolic links, and `/data` cannot store them. In `src/install/bin.rs`, an `EOPNOTSUPP` from `symlink` takes the exit Bun already has for Android shared storage: one warning, the bin links are skipped, the packages themselves are installed. `bun add semver` passed that way.

These had to work before install could:

- `__wrap_fcntl` builds `F_DUPFD_CLOEXEC` out of `F_DUPFD` and `F_SETFD`, because the kernel returns `EOPNOTSUPP` for `F_DUPFD_CLOEXEC` on every descriptor (`probe_fcntl`). `Bun.$` needed this.
- `__wrap_getaddrinfo` resolves a numeric service without a socket type. The console's resolver answers `EAI_SERVICE` otherwise, so `dns.lookup` failed.
- DNS uses the system resolver. c-ares finds no `/etc/resolv.conf` and asks `127.0.0.1`, which is `DNS_ECONNREFUSED`. The console's own resolver is used; its DNS server is not changed.
- [tooling/bun-port/ps5compat/syscall_errno.S](../tooling/bun-port/ps5compat/syscall_errno.S) wraps 33 SDK libc stubs that are `syscall; ret` with no carry check. The kernel's error number came back as a success: `openat("/etc/ssl/cert.pem")` returned 2 (`ENOENT`), Bun read that "descriptor" (stderr, the host socket) and HTTPS hung (`probe_openat`). The wrapper calls the stub and, when carry is set, sets `errno` and returns -1. It adds no `syscall` instruction. `syscall`, `ptrace` and `posix_fadvise` are left alone.
- `readdir` under `/data` uses a 128 KiB buffer. That file system refuses a `getdents` buffer smaller than its 64 KiB `st_blksize` (`EINVAL`); `/` accepts 512 bytes (`probe_getdents`). Elsewhere the buffer stays 8 KiB.

## What install still does not do

- Commands in `node_modules/.bin` are not created. The package loads through `require`. `bun run semver`, and `bunx` for those commands, do not work. `semver` above is the example.
- `bun install -g` still fails when it tries to create links in the global bin directory.
- Small files that invoke the package, in place of those symlinks, would be new Bun behaviour. It is not part of this cut.

## Other difficulties

The PS5 payload SDK is a FreeBSD 11 ABI. Bun and current Rust expect a newer one.

- Versioned libc symbols (`name@FBSD_1.0`) are stripped from a copy of the `libc` crate, because the SDK's unversioned stubs cannot satisfy them. Rust std is patched so `d_fileno` (`u32` in FreeBSD 11) widens to `u64` ([tooling/bun-port/patches/rust-std-ps5.patch](../tooling/bun-port/patches/rust-std-ps5.patch)). `AT_USRSTACKLIM` is absent. The SDK's `clock_nanosleep` is a stub for a FreeBSD 13 system call and returns `ENOSYS`; the wrapper maps it onto `nanosleep`. A normal exit comes back with `WCOREFLAG` (`0x80`) set, which Rust's `ExitStatus::success` treats as failure, so `waitpid` and `wait4` clear that bit on an exit status.
- The loader does not set up ELF TLS, so the Rust target uses emulated TLS. [tooling/bun-port/ps5compat/emutls.c](../tooling/bun-port/ps5compat/emutls.c) keeps a thread's TLS block alive for the first pthread destructor rounds. The SDK's compiler-rt frees it on the first round, before C++ and Rust `thread_local` destructors run (`probe_cxx`, `probe_keys`, `probe_rust`). `__cxa_thread_atexit_impl` covers the same hole in libc++abi. Destructors of the main thread's `thread_local` objects do not run at exit.
- Sony's clang driver forces unified full LTO, which cannot take Rust's ThinLTO inputs. The link uses the FreeBSD ELF driver, the SDK startup objects and linker script, PIE and 16 KiB alignment. The generic Bun ELF export must not require the FreeBSD CRT-only `environ` and `__progname` symbols.
- A payload cannot `fork` or `vfork` (both return `ENOSYS`). New processes exist only through `posix_spawn`, from shsrv's `elfldr_spawn`. [third_party/shsrv/elfldr.h](../third_party/shsrv/elfldr.h) adds `elfldr_spawn_env` so the launcher can pass an environment and a working directory: `PS5TERM_TTY`, `BUN_INSTALL`, `PATH` and `BUN_JSC_useJIT`.
- There is no pseudo-terminal. `openpty` returns `ENOSYS`. `ttyname_r` returns `ENOTTY` for a socket. `isatty` accepts the tab's socket when `PS5TERM_TTY=1`. Anything in oh-my-pi that needs a real PTY stays unavailable.
- A raw system call number can land on an unrelated Sony call. [tooling/bun-port/audit_syscalls.py](../tooling/bun-port/audit_syscalls.py) checks the binary against [tooling/bun-port/allowed_syscalls.json](../tooling/bun-port/allowed_syscalls.json). `recvmsg` (27), `sendmsg` (28), `select` (93) and `socket` (97) were added after checking the SDK's `sys/syscall.h` and the SDK libc that already issues them.
- `eventfd` is a pipe; `dup` of that descriptor is not supported. `getrandom` reads `/dev/urandom`. Sony's `pthread_getname_np` takes a 32-byte buffer and no size, so the wrapper copies into the caller's buffer. `initgroups` reads `/etc/group` before `setgroups`. A missing database fails the call and does not change groups. That function has been compiled and has not been exercised on the console.
- Upstream FreeBSD checks still disagree with this payload on SDK shared libraries, exception personality, TLS initialization and segment layout. Those checks have not been adapted or waived.
- The crash handler prints `PS5 crash:`, `rip`, `rsp` and 600 stack words before metadata that hangs after `Args:` when stderr is a socket. The PS5 `mcontext` has six more words than FreeBSD's before `mc_trapno`, so `rip` is ucontext word 28 and `rsp` word 31. To see where a hung process waits: `kill -s SEGV <pid>` through shsrv, then `llvm-addr2line -e bun-profile` on each stack word minus load base `0x200004000`.

## Next: the PS5Term tab

The shell already has the shape of the integration. `bun` and `omp` in [src/builtin_shell.cpp](../src/builtin_shell.cpp) call [src/platform/bun_install.cpp](../src/platform/bun_install.cpp), which copies `/app0/assets/bun` to `/data/ps5term/bun` because a payload started through the ELF loader cannot see `/app0`. The copy is streamed (`bun.elf` is about 120 MiB stripped in the first link, and the tree around it is hundreds of MiB), each file is written as `NAME.partial` and renamed, and `/data/ps5term/bun/.installed` is written last so an interrupted copy is redone. `assets/bun/` is gitignored.

That path has not been run on the console. The diary still treats the tab command and the first-use copy as the next step. Nothing here claims that a PS5Term tab has run `bun install`.

Intended interface, own tabs only, same rules as `run` (an ELF loader on port 9021, no pipes or redirection):

```text
/data$ bun --version
/data$ bun -e "console.log(require('os').platform())"
```

`bun ARGS` runs `/data/ps5term/bun/bin/bun.elf ARGS`. The launcher sets `PS5TERM_TTY=1`, `BUN_INSTALL=/data/ps5term/bun`, `PATH=/data/ps5term/bun/bin` and `BUN_JSC_useJIT=0`. `HOME` stays `/data`.

## Later: oh-my-pi on the console

One plan is to run the oh-my-pi coding agent on that same Bun, natively, in a PS5Term tab. `omp` here is `@oh-my-pi/pi-coding-agent` 19.0.1 (commit `f40f335845f0b136453d12fec8d6057011082766` in `sources.json`), not OpenMP. It is a harness, not a compiler runtime.

The diary marks the addon, the global install tree and the `omp` command as out of scope for the current cut. They stay a plan:

- `omp ARGS` would run `bun.elf` on `/data/ps5term/bun/install/global/node_modules/@oh-my-pi/pi-coding-agent/dist/cli.js`.
- The native addon is linked into `bun.elf`. `dlopen` of a path ending in `pi_natives.freebsd-x64-baseline.node` returns that addon. [tooling/bun-port/flatten_tree.py](../tooling/bun-port/flatten_tree.py) drops the other platforms' native packages.
- Settings would live in `/data/.omp`, because `HOME` is `/data`.
- Features that need a pseudo-terminal are unavailable. The kernel has no PTY driver.
- `bun install -g` still fails on the global bin symlinks, so installing oh-my-pi cannot depend on that command until there is another way to publish the CLI.
