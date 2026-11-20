# psbun

<p align="center">
  <img src="docs/images/ps5-bun.jpg" alt="psbun, Bun station 5 port" width="360" />
</p>

<p align="center">
  <strong>psbun</strong> (Bun station 5 port) is a native
  <a href="https://bun.sh">Bun</a> 1.4.4 payload for a jailbroken PlayStation 5.<br />
  By <a href="https://github.com/GabrielDuarteMG"><strong>Gabriel Duarte</strong> (@GabrielDuarteMG)</a>.
</p>

Bun runs here on the console's own FreeBSD (the FreeBSD 11 ABI of ps5-payload-sdk v0.45), not inside a Linux guest. The Rust target is `x86_64-ps5-freebsd`. C and C++ are `x86_64-sie-ps5` / `__PROSPERO__`. The full narrative is [docs/BUN_PORT.md](docs/BUN_PORT.md). The dated console log is [tooling/bun-port/README.md](tooling/bun-port/README.md).

This repository is a fork of [PS5Term](https://github.com/phantomptr/ps5term) by **PhantomPtr**. The terminal app in the tree is still PS5Term. psbun is the Bun port on top of it. Thank you, PhantomPtr, for PS5Term and for making this fork possible. The people PhantomPtr thanks are thanked again below. None of this stands without their work.

## What has run on a console

Every result is from the development launcher, against `/data/ps5term-dev/bun.elf`. The `bun` command inside a PS5Term tab has not been the thing under test.

The first `--version` never printed. The kernel log, hidden at first by the launcher's `[launch]` filter, was a `SIGSEGV` in `_mi_auto_process_init`. The PS5 loader does not set up ELF TLS, so mimalloc's `__thread` default heap was still the empty sentinel and the faulting read was at offset `0x28`. Building mimalloc with `MI_TLS_MODEL_PTHREADS=1` got past that. A second thread then crashed because `%fs:0` is not a unique per-thread pointer here. `MI_PRIM_THREAD_ID()` is `pthread_self()` under `__PROSPERO__`.

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

The syscall audit reports zero failures. `elfldr`, `shsrv` and `klogsrv` were still running afterwards.

## Install

How to install psbun on a console is coming soon.

## Why `bun install` needed a layer of its own

Nothing already published solves this. Bun asks the kernel for a function the PS5 kernel does not have, and reading kernel memory to recover it is out of the question. Each failure was measured on the console (`probe_fdpath`, `probe_faccessat`) before the workaround.

- **Path of an open file.** Bun uses `fcntl(F_KINFO)`, which exists from FreeBSD 13. On FreeBSD 11 the documented way is `sysctl kern.proc.filedesc`. On the console the first returns `EINVAL` and the second `ENOENT`: Sony removed that sysctl node. [ps5compat/fdpath.c](tooling/bun-port/ps5compat/fdpath.c) remembers the path passed to `open`, `openat`, `dup`, `dup2` and `fcntl(F_DUPFD*)`, and returns it only while `stat` and `fstat` still agree on device and inode. Without that, `bun install` stopped at `failed to resolve cache dir … EINVAL`.
- **`/data` has no hard links and no symbolic links.** Both return `EOPNOTSUPP`. An `EOPNOTSUPP` from `linkat` is treated like `EXDEV`, so Bun copies the package, the path it already has for other file systems that cannot link.
- **`fs.copyFileSync` returned `ENOSYS`.** The kernel has no `copy_file_range`, and Bun's FreeBSD read/write fallback only runs for `EXDEV`, `EINVAL`, `EOPNOTSUPP` and `EBADF`. ps5compat's `copy_file_range` is a read/write loop with the system call's offset and return rules.
- **`faccessat` and `readlinkat` always return `ENOSYS`.** `utimensat` returns `EACCES` and `futimens` returns `ESRCH` ("process not found"), which is a sign the call may be landing on a different Sony function. The layer never issues those calls. It uses `fstatat`, `access`, `readlink`, `utimes`, `lutimes` and `futimes`. Payloads run with real uid 1 and effective uid 0. This is what let `bun install --force` get past `the downloaded package was not found in the cache`. `/data` (nullfs) ignores access times even for the kernel's own `utimes`.
- **Package executables need a symlink.** `node_modules/.bin` entries are symbolic links, and `/data` cannot store them. An `EOPNOTSUPP` from `symlink` takes the exit Bun already has for Android shared storage: one warning, the bin links are skipped, the packages themselves are installed. `bun add semver` passed that way.

Before install could pass, a few more holes had to close. `F_DUPFD_CLOEXEC` returns `EOPNOTSUPP` on every descriptor, so it is built from `F_DUPFD` plus `F_SETFD` (`Bun.$` needed this). The console's `getaddrinfo` refuses a numeric service without a socket type. c-ares finds no `/etc/resolv.conf` and asks `127.0.0.1`, so DNS uses the console's own resolver. Thirty-three SDK libc stubs are `syscall; ret` with no carry check, so a kernel error came back as a success and HTTPS hung reading stderr. `readdir` under `/data` needs a 128 KiB buffer, because that file system refuses a `getdents` buffer smaller than its 64 KiB `st_blksize`.

The checked-in patches and `tools/build-bun-port.sh` do not yet reproduce the binary that ran. Bun source changes live in `.deps/bun-port/bun`, and the build environment in `build/bun-port/bunenv.sh`. Those directories are not in the repository.

## What it still does not do

- Commands in `node_modules/.bin` are not created. The package loads through `require`. `bun run semver`, and `bunx` for those commands, do not work.
- `bun install -g` still fails when it tries to create links in the global bin directory.
- Small files that invoke the package, in place of those symlinks, would be new Bun behaviour. That is not part of this cut.
- A PS5Term tab has not run `bun install`. The shell already has the shape of that integration (`bun` copies `/app0/assets/bun` to `/data/ps5term/bun`, because a payload cannot see `/app0`), and that copy has not been the console gate. The launcher would set `PS5TERM_TTY=1`, `BUN_INSTALL`, `PATH` and `BUN_JSC_useJIT=0`. `HOME` stays `/data`.

## Other difficulties

The shape of a payload this large comes from [ProsperoWeb](https://github.com/kvaps/ProsperoWeb): the open SDK, an ELF loader on port 9021, 16 KiB pages, a static link. Thank you to kvaps and everyone on ProsperoWeb. The WebKit that is compiled is Bun's fork, not ProsperoWeb's tree. ProsperoWeb turns the JavaScript JIT on. psbun compiles the JIT tiers (Bun's WebKit does not build with `ENABLE_JIT=OFF`) and then forces the LLInt interpreter. No executable memory is allocated for JIT code.

- Versioned libc symbols (`name@FBSD_1.0`) do not match the SDK's unversioned stubs. `d_fileno` is `u32` on FreeBSD 11. `AT_USRSTACKLIM` is absent. The SDK's `clock_nanosleep` is a FreeBSD 13 stub and returns `ENOSYS`. A normal exit comes back with `WCOREFLAG` (`0x80`) set, which Rust treats as failure.
- Emulated TLS has to outlive the first pthread destructor rounds, or C++ and Rust `thread_local` destructors never run. Sony's clang driver forces unified full LTO, which cannot take Rust's ThinLTO inputs, so the link uses the FreeBSD ELF driver.
- A payload cannot `fork` or `vfork`. New processes exist only through `posix_spawn`. There is no pseudo-terminal: `openpty` returns `ENOSYS`.
- A raw system call number can land on an unrelated Sony call. [audit_syscalls.py](tooling/bun-port/audit_syscalls.py) is the check.

The rest, including the crash handler and the load base used to symbolize it, is in [docs/BUN_PORT.md](docs/BUN_PORT.md).

## Later: oh-my-pi

One plan is to run the oh-my-pi coding agent on this same Bun, natively, in a PS5Term tab. `omp` here is `@oh-my-pi/pi-coding-agent` 19.0.1, not OpenMP. It is a harness. Its addon, global install tree and `omp` command are not in the current console cut. Settings would live in `/data/.omp`. Features that need a pseudo-terminal stay unavailable, and `bun install -g` still cannot publish the CLI.

## Thanks

Thank you, **PhantomPtr**, for [PS5Term](https://github.com/phantomptr/ps5term). This fork exists because you built the terminal, the launcher path and the place a payload can sit on the console's screen. The original repository, its license and its credits stay the foundation of everything below.

PS5Term, and so psbun, builds on other people's work. Thank you to each of them. **Included** means their source is in this repository or built into the app. **Derived** means their code was adapted. **Library** means a dependency the build pulls in. **Fetched on request** means it is downloaded from its own home when you use the feature, and is not shipped here. **Reference** means it was read to learn how something works, and no code was taken. Full notices are in [NOTICE.md](NOTICE.md).

**Included.** Thank you.

* [libvterm](https://www.leonerd.org.uk/code/libvterm/) (Paul Evans): the terminal emulation, in [`third_party/libvterm`](third_party/libvterm)
* [wolfSSH](https://github.com/wolfSSL/wolfssh) and [wolfSSL](https://github.com/wolfSSL/wolfssl) (wolfSSL Inc.): the SSH and SFTP server and its cryptography
* [TinyEMU](https://bellard.org/tinyemu/) (Fabrice Bellard): the RISC-V machine that runs Linux, in [`third_party/tinyemu`](third_party/tinyemu)
* [libslirp](https://gitlab.freedesktop.org/slirp/libslirp) (the QEMU project and contributors): the Linux guest's network, in [`third_party/libslirp`](third_party/libslirp)
* The Linux guest, built with [Buildroot](https://buildroot.org/): [Linux](https://kernel.org/), [OpenSBI](https://github.com/riscv-software-src/opensbi), [musl](https://musl.libc.org/), [BusyBox](https://busybox.net/), [ncurses](https://invisible-island.net/ncurses/), curl, Mbed TLS, nano, tmux, htop, jq, less, tree and e2fsprogs, and the people who maintain each of them
* [JetBrains Mono](https://www.jetbrains.com/lp/mono/) (JetBrains): the terminal font, in [`third_party/jetbrains-mono`](third_party/jetbrains-mono)
* [Noto Sans](https://github.com/notofonts) SC, TC, JP, KR, Arabic, Devanagari, Bengali and Thai (Adobe, Google and the Noto Project Authors): the app's translated text
* [font8x8](https://github.com/dhepper/font8x8) (Daniel Hepper): the public-domain bitmap font, in [`third_party/font8x8`](third_party/font8x8)
* The icon, selection artwork and the music track "Night Drive" by BlackBearReloaded

**Derived.** Thank you.

* ps5-native-app-boilerplate (BlackBearReloaded): the native app foundation
* [shsrv](https://github.com/ps5-payload-dev/shsrv) (John Törnblom): the local command backend, in [`third_party/shsrv`](third_party/shsrv). psbun starts processes through this same `elfldr`
* [ps5sshd](https://github.com/intestinate/ps5sshd) (intestinate): the legacy SSH service, in [`third_party/ps5sshd`](third_party/ps5sshd)

**Libraries.** Thank you.

* [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) (John Törnblom), with its LLVM libc++ headers, and [zlib](https://zlib.net/). The whole port is built on this SDK
* [GoogleTest](https://github.com/google/googletest) for host tests only
* [Bun](https://github.com/oven-sh/bun) (Oven and the Bun contributors): the runtime this port is trying to run
* [ProsperoWeb](https://github.com/kvaps/ProsperoWeb) (kvaps and contributors): the example of a large WebKit payload on this SDK

**Fetched on request.** Thank you.

* [Alpine Linux](https://www.alpinelinux.org/) minirootfs and the `apk` packages you install
* [Ubuntu Base](https://cdimage.ubuntu.com/ubuntu-base/) 24.04 and the `apt` packages you install
* Optional packaging tools: [UFS2Tool](https://github.com/SvenGDK/UFS2Tool) (SvenGDK), [MkPFS](https://github.com/PSBrew/MkPFS) (PSBrew) and [PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo) ports
* [GNU Bash](https://www.gnu.org/software/bash/) for the optional, uninstalled Bash port

**References.** Thank you.

* [kstuff-lite](https://github.com/EchoStretch/kstuff-lite) (EchoStretch and the kstuff contributors): the native-executable capability record for one research profile
* [kodi-ps5](https://github.com/Phoenixx1202/kodi-ps5) (Phoenixx1202 and contributors): the native directory buffer requirement
* SharpProspero: the PS5 ELF format, while writing PS5Term's own converter
* [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain): the public libkernel mapping declarations

Thank you to the rest of the PS5 scene as well: the people behind the loaders, kernel patches, payloads and file managers. PS5Term is one tool among many, and psbun is one more attempt on top of it. If something here is useful, the thanks belong to them first.

## License

psbun follows PS5Term: free software under the GNU General Public License v3.0 or later. See [LICENSE](LICENSE). Included third-party code keeps its own license, next to the code or in the app's `licenses` folders. See [NOTICE.md](NOTICE.md).

> psbun and PS5Term are not affiliated with, endorsed by, or connected to Sony Interactive Entertainment Inc. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment Inc., used here for identification only. Use is at your own risk. Full terms: [DISCLAIMER.md](DISCLAIMER.md).
