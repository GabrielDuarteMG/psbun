// PS5Term - copy the bundled Bun into /data, where payloads can read it. GPL-3.0-or-later.
#include "bun_install.hpp"
#include "directory.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <new>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace term::platform
{
namespace
{
constexpr std::size_t copy_buffer_size = 256 * 1024, version_limit = 1024;
constexpr unsigned max_depth = 64;
using Path = std::array<char, 1024>;

bool format_path(Path &path, const char *base, const char *suffix) noexcept
{
    const int size = std::snprintf(path.data(), path.size(), "%s%s", base, suffix);
    if (size < 0 || static_cast<std::size_t>(size) >= path.size())
    {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}
bool write_all(int fd, const char *bytes, std::size_t size) noexcept
{
    while (size)
    {
        const auto wrote = ::write(fd, bytes, size);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
        {
            if (wrote == 0)
                errno = EIO;
            return false;
        }
        bytes += wrote;
        size -= static_cast<std::size_t>(wrote);
    }
    return true;
}
// A whole small file (the version and its marker); larger than the buffer is EFBIG.
bool read_small(const char *path, std::array<char, version_limit> &bytes,
                std::size_t &size) noexcept
{
    const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    size = 0;
    bool done = false, failed = false;
    while (!done && !failed)
    {
        if (size == bytes.size())
        {
            errno = EFBIG;
            failed = true;
            break;
        }
        const auto got = ::read(fd, bytes.data() + size, bytes.size() - size);
        if (got < 0 && errno == EINTR)
            continue;
        failed = got < 0;
        done = got == 0;
        if (got > 0)
            size += static_cast<std::size_t>(got);
    }
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return !failed;
}
// Written whole as TARGET.partial, then renamed: a reader never sees half a file.
int open_partial(const char *target, Path &partial) noexcept
{
    if (!format_path(partial, target, ".partial"))
        return -1;
    const int fd =
        ::open(partial.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd >= 0 && ::fchmod(fd, 0644) != 0)
    {
        const int saved = errno;
        ::close(fd);
        ::unlink(partial.data());
        errno = saved;
        return -1;
    }
    return fd;
}
// written false: errno already says why.
bool finish_partial(int fd, bool written, const Path &partial, const char *target) noexcept
{
    int failure = written ? 0 : errno;
    if (::close(fd) != 0 && !failure)
        failure = errno;
    if (!failure && ::rename(partial.data(), target) != 0)
        failure = errno;
    if (!failure)
        return true;
    ::unlink(partial.data());
    errno = failure;
    return false;
}
bool install_file(int source, const char *target, char *buffer) noexcept
{
    Path partial;
    const int out = open_partial(target, partial);
    if (out < 0)
        return false;
    bool written = true;
    for (;;)
    {
        const auto got = ::read(source, buffer, copy_buffer_size);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
        {
            written = got == 0;
            break;
        }
        if (!write_all(out, buffer, static_cast<std::size_t>(got)))
        {
            written = false;
            break;
        }
    }
    return finish_partial(out, written, partial, target);
}
bool install_marker(const char *target, const char *bytes, std::size_t size) noexcept
{
    Path partial;
    const int out = open_partial(target, partial);
    return out >= 0 && finish_partial(out, write_all(out, bytes, size), partial, target);
}
// Read by the program, which runs as another user.
bool make_directory(const char *path) noexcept
{
    if (::mkdir(path, 0755) != 0)
    {
        if (errno != EEXIST)
            return false;
        struct stat st
        {
        };
        if (::stat(path, &st) != 0)
            return false;
        if (!S_ISDIR(st.st_mode))
        {
            errno = ENOTDIR;
            return false;
        }
    }
    return ::chmod(path, 0755) == 0;
}
// The folders above target, as `mkdir -p` does; existing ones are left as they are.
bool make_parents(const char *target) noexcept
{
    Path path;
    if (!format_path(path, target, ""))
        return false;
    for (char *slash = std::strchr(path.data() + 1, '/'); slash;
         slash = std::strchr(slash + 1, '/'))
    {
        *slash = '\0';
        struct stat st
        {
        };
        const bool present = ::stat(path.data(), &st) == 0;
        if (present && !S_ISDIR(st.st_mode))
        {
            errno = ENOTDIR;
            return false;
        }
        if (!present && ::mkdir(path.data(), 0755) != 0 && errno != EEXIST)
            return false;
        *slash = '/';
    }
    return true;
}
bool copy_tree(Path &source, Path &target, char *buffer, unsigned depth) noexcept;
bool copy_entry(Path &source, Path &target, char *buffer, unsigned depth) noexcept
{
    // No-follow descriptor, not lstat: native app contexts can reject lstat on readable
    // files. Nonblocking, so a FIFO cannot stall the copy.
    const int fd = ::open(source.data(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return errno == ELOOP || errno == EMLINK; // a symbolic link: skipped
    struct stat st
    {
    };
    bool copied = ::fstat(fd, &st) == 0;
    if (copied && S_ISREG(st.st_mode))
        copied = install_file(fd, target.data(), buffer);
    const int saved = errno;
    ::close(fd);
    errno = saved;
    if (copied && S_ISDIR(st.st_mode))
        return copy_tree(source, target, buffer, depth + 1);
    return copied; // Other special files are skipped.
}
bool copy_tree(Path &source, Path &target, char *buffer, unsigned depth) noexcept
{
    if (depth > max_depth)
    {
        errno = ELOOP;
        return false;
    }
    if (!make_directory(target.data()))
        return false;
    // One per level, on the heap: a PS5 cursor holds a 64 KiB block buffer.
    const std::unique_ptr<DirectoryCursor> cursor(new (std::nothrow) DirectoryCursor);
    if (!cursor)
    {
        errno = ENOMEM;
        return false;
    }
    if (!cursor->open(source.data(), true))
        return false;
    const std::size_t source_length = std::strlen(source.data()),
                      target_length = std::strlen(target.data());
    for (;;)
    {
        std::string_view name;
        const auto step = cursor->next(name);
        if (step == DirectoryStep::end)
            return true;
        if (step == DirectoryStep::failed)
            return false;
        if (name == "." || name == "..")
            continue;
        if (source_length + 1 + name.size() >= source.size() ||
            target_length + 1 + name.size() + sizeof ".partial" > target.size())
        {
            errno = ENAMETOOLONG;
            return false;
        }
        source[source_length] = target[target_length] = '/';
        std::memcpy(source.data() + source_length + 1, name.data(), name.size());
        std::memcpy(target.data() + target_length + 1, name.data(), name.size());
        source[source_length + 1 + name.size()] = target[target_length + 1 + name.size()] = '\0';
        const bool copied = copy_entry(source, target, buffer, depth);
        source[source_length] = target[target_length] = '\0';
        if (!copied)
            return false;
    }
}
bool install(const char *app_root, const char *target) noexcept
{
    if (!app_root || !target || app_root[0] != '/' || target[0] != '/')
    {
        errno = EINVAL;
        return false;
    }
    Path version_path, marker_path;
    std::array<char, version_limit> version{}, marker{};
    std::size_t version_size = 0, marker_size = 0;
    if (!format_path(version_path, app_root, "/version") ||
        !format_path(marker_path, target, "/.installed") ||
        !read_small(version_path.data(), version, version_size))
        return false;
    if (read_small(marker_path.data(), marker, marker_size) && marker_size == version_size &&
        std::memcmp(marker.data(), version.data(), version_size) == 0)
        return true;
    Path source, destination;
    if (!format_path(source, app_root, "") || !format_path(destination, target, "") ||
        !make_parents(target))
        return false;
    // Gone until the copy completes, so an interrupted reinstall is redone too.
    if (::unlink(marker_path.data()) != 0 && errno != ENOENT)
        return false;
    const std::unique_ptr<char[]> buffer(new (std::nothrow) char[copy_buffer_size]);
    if (!buffer)
    {
        errno = ENOMEM;
        return false;
    }
    return copy_tree(source, destination, buffer.get(), 0) &&
           install_marker(marker_path.data(), version.data(), version_size);
}
} // namespace

bool ensure_bun_installed(const char *app_root, const char *target, int *error) noexcept
{
    errno = 0;
    const bool installed = install(app_root, target);
    if (!installed && error)
        *error = errno ? errno : EIO;
    return installed;
}
} // namespace term::platform
