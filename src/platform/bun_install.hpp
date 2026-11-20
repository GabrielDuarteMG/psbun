// PS5Term - copy the bundled Bun into /data, where payloads can read it. GPL-3.0-or-later.
// The app ships Bun (bun.elf and the omp install tree) in /app0/assets/bun, which a payload
// started through the ELF loader cannot see; `bun` and `omp` copy it to /data/ps5term/bun.
#pragma once

namespace term::platform
{
inline constexpr const char *bun_app_root = "/app0/assets/bun";
inline constexpr const char *bun_target = "/data/ps5term/bun";

// Copies app_root into target unless target/.installed already holds app_root/version.
// Files are streamed (bun.elf is hundreds of MiB), each written as NAME.partial and then
// renamed; folders are 0755 and files 0644, since the program runs as another user. The
// marker is written last, so an interrupted copy is redone next time. Symbolic links and
// special files are skipped. On failure *error (when given) holds the errno.
bool ensure_bun_installed(const char *app_root = bun_app_root, const char *target = bun_target,
                          int *error = nullptr) noexcept;
} // namespace term::platform
