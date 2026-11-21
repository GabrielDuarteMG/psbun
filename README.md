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

## Bun feature progress

**Initial console-validation progress: 17.5% (7 of 40 feature families have a documented passing smoke).** This measures breadth of recorded validation, not full Bun compatibility, implementation completion, reliability, or release readiness. Every family has equal weight and a passing subset counts once; every partial, blocked and unexplored family stays in the denominator. No whole family is certified complete by this score.

Inventory: the live [Bun docs](https://bun.sh/docs) and [feature index](https://bun.sh/llms.txt), reviewed on 2026-11-20, compared with this port's Bun 1.4.4 records. Results below are inherited from the development-launcher log, whose later runtime section is labeled 2026-11-21; no console tests were run for this assessment. Newer live-doc APIs still need version/availability checks against the pinned source.

`100 × count(P) / count(all rows) = 100 × 7 / 40 = 17.5%`. When updating later, change the row evidence first, then recompute the number. Keep unknowns in the denominator. This is a grouped checklist, not a count of documentation pages, individual methods, guides, or equal engineering effort.

- `P — Partial smoke`: at least one directly recorded successful console scenario; remaining coverage is explicit.
- `M — Mapped, unvalidated`: known source, configuration, limitation or prerequisite; no direct family-level console pass.
- `B — Blocked`: documented failure or explicitly unsupported path.
- `U — Unexplored in this audit`: no port-specific validation evidence in README.md, [docs/BUN_PORT.md](docs/BUN_PORT.md), `tooling/bun-port/`, `tests/`, `tools/build-bun-port.sh` or `tools/dev/ps5_bun_console.py`. Establish availability, then test. U does not mean "not implemented."

| ID | Feature family | Status | Evidence / current limit | Still to implement or validate |
| --- | --- | --- | --- | --- |
| F01 | [JavaScript CLI and console](https://bun.sh/docs/runtime) | P | [Runtime log](tooling/bun-port/README.md#runtime-fixes-after-the-first-start-2026-11-21): version, eval, console and platform/arch output pass. LLInt only. | File entrypoints, package scripts, argv, exit/error behavior; no JIT/performance parity claim. |
| F02 | [TypeScript, JSX/TSX and Transpiler](https://bun.sh/docs/typescript) | U | No port-specific result located. | Typed entrypoint and JSX transformation with known output; direct `Bun.Transpiler` result. |
| F03 | [Modules, runtime plugins and auto-install](https://bun.sh/docs/runtime/module-resolution) | P | [Console results](docs/BUN_PORT.md#what-has-run-on-a-console): installed packages load through `require`. | ESM/CJS interop, dynamic import, exports, `import.meta`, loaders, `Bun.plugin` and auto-install. `require` is not proof for all of these. |
| F04 | [Environment and bunfig.toml](https://bun.sh/docs/runtime/bunfig) | M | [Tab integration](docs/BUN_PORT.md#next-the-ps5term-tab) maps launcher environment variables. dotenv and config precedence are not validated. | Validate cwd, `HOME`, argv and config precedence using dummy values only. |
| F05 | [Watch and hot reload](https://bun.sh/docs/runtime/watch-mode) | U | No port-specific result located. | A single file change triggers the expected reload and stops cleanly. Distinguish watch from hot state retention. |
| F06 | [Debugger and REPL](https://bun.sh/docs/runtime/debugger) | U | Native crash diagnostics are not JavaScript debugger or REPL validation. | Debugger attach and breakpoint, plus REPL evaluate and exit. Document terminal limitations. |
| F07 | [Bun Shell](https://bun.sh/docs/runtime/shell) | P | [Runtime log](tooling/bun-port/README.md#runtime-fixes-after-the-first-start-2026-11-21): shell echo passes. | Pipes, redirects, quoting, env and nonzero exits. Writes only in a future owned `/data` test fixture. |
| F08 | [Child processes and terminal](https://bun.sh/docs/runtime/child-process) | P | [Runtime log](tooling/bun-port/README.md#runtime-fixes-after-the-first-start-2026-11-21): basic spawn passes; nested spawn recorded 13/14 with an intermittent assertion. [Port limits](docs/BUN_PORT.md#other-difficulties): fork, vfork and PTY unavailable. [Install limits](docs/BUN_PORT.md#what-install-still-does-not-do): `process.execPath` always names the installed `bun.elf`. | `spawnSync`, stdio, cancellation, IPC, exit semantics, `execPath` override and bounded nested-spawn stability. Do not equate POSIX fork absence with `node:child_process.fork` by name. |
| F09 | [HTTP client and TLS](https://bun.sh/docs/runtime/networking/fetch) | P | [Runtime log](tooling/bun-port/README.md#runtime-fixes-after-the-first-start-2026-11-21): two HTTPS 200 responses and authorized TLSv1.3 via `node:tls`. | Request bodies, redirects, cancellation, streaming, invalid-certificate rejection and error handling. No server-TLS claim. |
| F10 | [DNS](https://bun.sh/docs/runtime/networking/dns) | M | [Runtime log](tooling/bun-port/README.md#runtime-fixes-after-the-first-start-2026-11-21) maps resolver fixes. HTTPS success is indirect evidence. | Explicit lookup result and controlled lookup failure. `Bun.dns` record and cache APIs separately. |
| F11 | [HTTP server, routing and cookies](https://bun.sh/docs/runtime/http/server) | U | No `Bun.serve` console result located. | Loopback request, status and body; routing, `Cookie`/`CookieMap`, `FileSystemRouter`, server TLS, errors, metrics and shutdown. |
| F12 | [WebSockets](https://bun.sh/docs/runtime/http/websockets) | U | No client or server WebSocket result located. | Local client/server message, close, pub/sub and backpressure. |
| F13 | [TCP and UDP](https://bun.sh/docs/runtime/networking/tcp) | U | A native connect probe is not proof of `Bun.listen`, `Bun.connect` or `Bun.udpSocket`. | Bounded local TCP and UDP echo, error and close checks. |
| F14 | [File I/O](https://bun.sh/docs/runtime/file-io) | P | [Console results](docs/BUN_PORT.md#what-has-run-on-a-console): `copyFileSync`, `utimesSync`, `accessSync` and `existsSync`. `/data` link and timestamp limits remain. | `Bun.file`, `Bun.write`, `FileSink`, async fs, directory iteration, missing-file errors and an exact byte roundtrip. |
| F15 | [Streams, binary data and Web APIs](https://bun.sh/docs/runtime/web-apis) | U | The fetch smoke does not establish the broader APIs. | Streams, backpressure and cancellation; `Buffer`, `ArrayBuffer`, `Blob`, `Request`, `Response`, `Headers`, `URL`, encoders, `FormData` and global behavior. |
| F16 | [Archives and compression](https://bun.sh/docs/runtime/archive) | U | No standalone API result located. Installer use is not API coverage. | Archive create/extract and gzip, deflate and zstd roundtrip, malformed input, and owned-path confinement. |
| F17 | [SQLite](https://bun.sh/docs/runtime/sqlite) | U | No `bun:sqlite` console result located. | In-memory create, insert, query and rollback, then persistence in an owned `/data` fixture. |
| F18 | [SQL, Redis and S3 clients](https://bun.sh/docs/runtime/sql) | U | No port-specific client result located. | Separate PostgreSQL, MySQL, Redis and S3 fixtures with known read, write and error results. No live credentials in docs. |
| F19 | [Workers](https://bun.sh/docs/runtime/workers) | U | Native pthread and TLS probes do not establish `Worker` or `node:worker_threads`. | Message exchange, transfer, termination, errors and bounded repeated startup. |
| F20 | [Broader Node.js compatibility](https://bun.sh/docs/runtime/nodejs-compat) | M | [Port limits](docs/BUN_PORT.md#other-difficulties) map OS and ABI gaps and untested `initgroups`. Named fs, tls, process and `require` smokes are counted in their own rows. | Module-by-module acceptance beyond those rows. Distinguish implemented wrappers, host cases and console results. |
| F21 | [Node-API addons](https://bun.sh/docs/runtime/node-api) | B | [Native loader](tooling/bun-port/ps5compat/ps5compat.c) rejects dynamic `.node` modules. The conditional static pi-natives bridge is not general addon support. | Establish an explicitly supported loading path before addon acceptance. omp remains outside the current console cut. |
| F22 | [FFI](https://bun.sh/docs/runtime/ffi) and [C compiler](https://bun.sh/docs/runtime/c-compiler) | M | Real `dlopen` delegation exists in the [native loader](tooling/bun-port/ps5compat/ps5compat.c). The inspected FreeBSD config defaults TinyCC off; flags of the tested binary are unverified. | Determine build availability first, then bounded calls and callbacks on an owned library. Keep `bun:ffi` separate from C compilation. |
| F23 | [Hashing, crypto, CSRF and secrets](https://bun.sh/docs/runtime/hashing) | U | No direct API result located. TLS is not proof of these APIs. | Known hash and password cases, Web Crypto, CSRF verification and secret-store availability. Dummy secrets only. |
| F24 | [Data parsers](https://bun.sh/docs/runtime/toml) | U | JSON, TOML, YAML, Markdown, JSON5, XML and JSONL have no standalone port validation located. | Establish version availability. Check known parse and render output, plus malformed input, for each. |
| F25 | [HTMLRewriter, Image and WebView](https://bun.sh/docs/runtime/html-rewriter) | U | No port-specific result located. A live-doc page is not port support. | Confirm availability, then a deterministic HTML transform and image fixture. Establish a browser backend before WebView tests. |
| F26 | [Glob and utilities](https://bun.sh/docs/runtime/utils) | U | An installed semver package does not validate `Bun.semver`. | Glob match set, semver, color, timing, UUID, inspection, equality, text, path and URL, resolution, memory and GC utilities against explicit expected outputs. |
| F27 | [WebAssembly](https://bun.sh/docs/runtime/web-apis) | U | No WASM result located. JIT disabled does not prove WASM absent. | Establish availability and run a tiny fixed-function module with a known result. |
| F28 | [Local install, add, cache and lockfile](https://bun.sh/docs/pm/cli/install) | P | [Console results](docs/BUN_PORT.md#what-has-run-on-a-console): install, `--force` and `add semver`, plus `require`, pass, with the copy fallback. | Frozen, offline, cache and lockfile behavior, package variants and repeatability. Bin publication belongs to F29. |
| F29 | [Package CLI bins, global installs and bunx](https://bun.sh/docs/pm/bunx) | M | [Install limits](docs/BUN_PORT.md#what-install-still-does-not-do): the shim is the current design. `bun add -g` was checked only with a writable bin directory. The default app bin stays `EACCES`. [Shim fixtures](tests/test_bun_bin_shim.py) are host-only. No recorded `bun run` or `bunx` output. | Run one local shim and one `bunx` command with expected stdout and exit, plus rejection of a foreign global-bin file. Do not claim the default app bin is writable. |
| F30 | [Workspaces and link-based installs](https://bun.sh/docs/pm/workspaces) | M | `/data` cannot store symlinks. `bun link`, `bun unlink`, workspace links, isolated installs and the global virtual store are unvalidated. The risk is inferred from documented link use, not from an observed test failure. | Check each topology against the no-symlink filesystem and verify dependency resolution, including catalogs and `--filter`. |
| F31 | [Package maintenance and publishing](https://bun.sh/docs/pm/cli/pm) | U | No port-specific command result located. | `remove`, `update`, `dedupe`, `prune`, `outdated`, `why`, `info`, `audit`, `pm`, `patch`, `pack` and `publish`. Use controlled fixtures or dry-run. Never publish a real package for acceptance. |
| F32 | [Package policy and configuration](https://bun.sh/docs/pm/lifecycle) | U | A basic registry install does not prove every configuration or policy path. | Lifecycle trust, registries, scopes, `.npmrc`, overrides, resolutions, the security scanner and alternate dependency sources, with dummy credentials. |
| F33 | [Core bundler](https://bun.sh/docs/bundler) | U | No `bun build` or `Bun.build` console result located. | Build a tiny JavaScript and TypeScript graph through the CLI and the API. Inspect and run the expected output, sourcemaps, loaders and errors. |
| F34 | [Frontend and fullstack bundling](https://bun.sh/docs/bundler/fullstack) | U | No port-specific result located. | HTML, static and standalone HTML, CSS, fullstack serving and HMR, using a tiny local fixture. |
| F35 | [Advanced bundling and executables](https://bun.sh/docs/bundler/executables) | U | No port-specific result located. Upstream executable targets do not establish PS5 support. | Splitting, minification, plugins, macros, bytecode and target-format availability. Validate payload and loader compatibility before any standalone executable claim. |
| F36 | [Test runner basics](https://bun.sh/docs/test) | U | Native and host project tests are not `bun:test` acceptance on the console. | One pass and one intentional failure, with expected exit codes, TypeScript discovery, an async timeout and hooks. |
| F37 | [Advanced testing](https://bun.sh/docs/test/mocks) | U | No port-specific result located. | Mocks, snapshots, fake dates and timers, coverage, reporters, a DOM dependency, preload, watch, concurrency, isolation and sharding, with deterministic fixtures. |
| F38 | [Project creation](https://bun.sh/docs/runtime/templating/init) | U | No `bun init` or `bun create` console result located. | Generate a small project in an owned development directory and run its entrypoint. Treat template and network dependencies separately. |
| F39 | [Type checker](https://bun.sh/docs/runtime/check), [linter](https://bun.sh/docs/lint) and [formatter](https://bun.sh/docs/format) | U | Current docs advertise `bun check`, `bun lint` and `bun fmt`. Availability in the pinned port is unverified. | Confirm each command is present. An intentional type error, lint error and formatting check must produce the documented diagnostic and exit. |
| F40 | [Cron](https://bun.sh/docs/runtime/cron) | U | Live-doc API. Pinned-port availability and execution are unverified. | Confirm the API is present, then a fixed next-occurrence result and bounded schedule and cancellation. |

### Mapped delivery gaps

These are project gates. They are not extra rows in the 40-family score. Package-bin acceptance, including the unwritable default app directory, stays in F29.

- [ ] Finish checked-in patch and build reproducibility of the console-tested binary. Local `.deps` and build state are not a reproducible deliverable. See [where the shape comes from](docs/BUN_PORT.md#where-the-shape-comes-from).
- [ ] Validate the first-use asset copy and a `bun` launch and install inside a PS5Term tab. See [tab integration](docs/BUN_PORT.md#next-the-ps5term-tab) and `tests/terminal/test_bun_install.cpp`. Those host fixtures do not prove console acceptance.
- [ ] Keep the known limits visible: LLInt with no JIT, no PTY, no POSIX fork or vfork, and main-thread TLS destructors that do not run at exit. See [port limits](docs/BUN_PORT.md#other-difficulties).
- [ ] Resolve or characterize nested-spawn reliability and the socket-backed crash-report hang. A passing basic spawn is not a stable-process gate. See the [runtime log](tooling/bun-port/README.md#runtime-fixes-after-the-first-start-2026-11-21).
- [ ] Adapt or resolve the outstanding upstream FreeBSD verification differences. A zero-failure syscall audit is static evidence, not full compatibility. See [port limits](docs/BUN_PORT.md#other-difficulties).

A new P status needs a recorded command or scenario, the expected result, the observed result, the runtime or build identity, and the console or launcher context. Host results stay labeled host-only. Without that evidence, the status stays unchanged.

## Install

How to install psbun on a console is coming soon.

## Why `bun install` needed a layer of its own

Nothing already published solves this. Bun asks the kernel for a function the PS5 kernel does not have, and reading kernel memory to recover it is out of the question. Each failure was measured on the console (`probe_fdpath`, `probe_faccessat`) before the workaround.

- **Path of an open file.** Bun uses `fcntl(F_KINFO)`, which exists from FreeBSD 13. On FreeBSD 11 the documented way is `sysctl kern.proc.filedesc`. On the console the first returns `EINVAL` and the second `ENOENT`: Sony removed that sysctl node. [ps5compat/fdpath.c](tooling/bun-port/ps5compat/fdpath.c) remembers the path passed to `open`, `openat`, `dup`, `dup2` and `fcntl(F_DUPFD*)`, and returns it only while `stat` and `fstat` still agree on device and inode. Without that, `bun install` stopped at `failed to resolve cache dir … EINVAL`.
- **`/data` has no hard links and no symbolic links.** Both return `EOPNOTSUPP`. An `EOPNOTSUPP` from `linkat` is treated like `EXDEV`, so Bun copies the package, the path it already has for other file systems that cannot link.
- **`fs.copyFileSync` returned `ENOSYS`.** The kernel has no `copy_file_range`, and Bun's FreeBSD read/write fallback only runs for `EXDEV`, `EINVAL`, `EOPNOTSUPP` and `EBADF`. ps5compat's `copy_file_range` is a read/write loop with the system call's offset and return rules.
- **`faccessat` and `readlinkat` always return `ENOSYS`.** `utimensat` returns `EACCES` and `futimens` returns `ESRCH` ("process not found"), which is a sign the call may be landing on a different Sony function. The layer never issues those calls. It uses `fstatat`, `access`, `readlink`, `utimes`, `lutimes` and `futimes`. Payloads run with real uid 1 and effective uid 0. This is what let `bun install --force` get past `the downloaded package was not found in the cache`. `/data` (nullfs) ignores access times even for the kernel's own `utimes`.
- **Package executables need a symlink.** `node_modules/.bin` entries are symbolic links, and `/data` cannot store them. An `EOPNOTSUPP` from `symlink` writes a bin shim: a 0755 `#!/usr/bin/env bun` file whose second line is `// bun-ps5-bin-shim <target>`. Bun reads that file back as a link, and `posix_spawn` runs the target in `bun.elf`. See [binshim.c](tooling/bun-port/ps5compat/binshim.c).

Before install could pass, a few more holes had to close. `F_DUPFD_CLOEXEC` returns `EOPNOTSUPP` on every descriptor, so it is built from `F_DUPFD` plus `F_SETFD` (`Bun.$` needed this). The console's `getaddrinfo` refuses a numeric service without a socket type. c-ares finds no `/etc/resolv.conf` and asks `127.0.0.1`, so DNS uses the console's own resolver. Thirty-three SDK libc stubs are `syscall; ret` with no carry check, so a kernel error came back as a success and HTTPS hung reading stderr. `readdir` under `/data` needs a 128 KiB buffer, because that file system refuses a `getdents` buffer smaller than its 64 KiB `st_blksize`.

The checked-in patches and `tools/build-bun-port.sh` do not yet reproduce the binary that ran. Bun source changes live in `.deps/bun-port/bun`, and the build environment in `build/bun-port/bunenv.sh`. Those directories are not in the repository.

## What it still does not do

- `bun add -g` was checked only with a writable `BUN_INSTALL_BIN` and `BUN_INSTALL_GLOBAL_DIR`. A payload cannot write the app directory `/data/ps5term/bun/bin` (`EACCES`: it belongs to uid 1, mode 755, and effective uid 0 is not treated as root).
- `process.execPath` is `/data/ps5term/bun/bin/bun.elf` even when another `bun.elf` is running, so `Bun.spawn(process.execPath, …)` starts the installed copy.
- Running a shim through `bun run` or `bunx` has no recorded console result. The package still loads through `require`.
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
