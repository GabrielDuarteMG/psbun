// PS5Term GPL-3.0-or-later. Copying the bundled Bun into /data: a streamed tree copy with
// the modes another user needs, the version marker, reinstalls and an unwritable target.
#include "platform/bun_install.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
void write_file(const std::string &path, const std::string &bytes, mode_t mode = 0600)
{
    FILE *file = std::fopen(path.c_str(), "wb");
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::fclose(file);
    assert(chmod(path.c_str(), mode) == 0);
}
std::string read_file(const std::string &path)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    assert(file);
    std::string bytes;
    char buffer[65536];
    for (std::size_t got; (got = std::fread(buffer, 1, sizeof buffer, file)) > 0;)
        bytes.append(buffer, got);
    std::fclose(file);
    return bytes;
}
mode_t mode_of(const std::string &path)
{
    struct stat st
    {
    };
    assert(lstat(path.c_str(), &st) == 0);
    return st.st_mode & 07777;
}
bool exists(const std::string &path)
{
    struct stat st
    {
    };
    return lstat(path.c_str(), &st) == 0;
}
bool install(const std::string &app, const std::string &target, int *error = nullptr)
{
    return term::platform::ensure_bun_installed(app.c_str(), target.c_str(), error);
}
} // namespace

int main()
{
    char directory[] = "/tmp/ps5term-bun-XXXXXX";
    assert(mkdtemp(directory));
    const std::string root = directory, app = root + "/app", target = root + "/data/ps5term/bun",
                      modules = "/install/global/node_modules";
    for (const char *folder : {"", "/bin", "/install", "/install/global",
                               "/install/global/node_modules", "/install/global/node_modules/a"})
        assert(mkdir((app + folder).c_str(), 0700) == 0);
    // Larger than the 256 KiB copy buffer, so the copy takes several reads.
    std::string elf = "\177ELF";
    for (std::size_t i = 0; elf.size() < 600 * 1024; ++i)
        elf += static_cast<char>('a' + i % 26);
    write_file(app + "/version", "bun-1.4.4+test\n");
    write_file(app + "/bin/bun.elf", elf, 0700);
    write_file(app + modules + "/a/b.js", "module.exports = 1;\n");
    assert(symlink((app + "/bin/bun.elf").c_str(), (app + "/bin/bun-link").c_str()) == 0);

    // First use: everything copied, folders 0755, files 0644, the marker last.
    int error = -1;
    assert(install(app, target, &error) && error == -1);
    assert(read_file(target + "/bin/bun.elf") == elf);
    assert(read_file(target + modules + "/a/b.js") == "module.exports = 1;\n");
    assert(read_file(target + "/.installed") == "bun-1.4.4+test\n");
    for (const char *folder : {"", "/bin", "/install", "/install/global",
                               "/install/global/node_modules", "/install/global/node_modules/a"})
        assert(mode_of(target + folder) == 0755);
    for (const char *file :
         {"/bin/bun.elf", "/install/global/node_modules/a/b.js", "/version", "/.installed"})
        assert(mode_of(target + file) == 0644);
    assert(!exists(target + "/bin/bun-link"));        // links are skipped
    assert(!exists(target + "/bin/bun.elf.partial")); // nothing half-written is left
    assert(!exists(target + "/.installed.partial"));

    // Same version: nothing is copied again.
    write_file(target + "/bin/bun.elf", "sentinel", 0644);
    assert(install(app, target));
    assert(read_file(target + "/bin/bun.elf") == "sentinel");

    // A new version reinstalls.
    write_file(app + "/version", "bun-1.4.4+next\n");
    assert(install(app, target));
    assert(read_file(target + "/bin/bun.elf") == elf);
    assert(read_file(target + "/.installed") == "bun-1.4.4+next\n");

    // An interrupted copy has no marker yet, so it is redone.
    write_file(target + "/bin/bun.elf", "sentinel", 0644);
    assert(unlink((target + "/.installed").c_str()) == 0);
    assert(install(app, target));
    assert(read_file(target + "/bin/bun.elf") == elf);

    // No version in the app: nothing to install.
    error = 0;
    assert(!install(root + "/missing", root + "/other", &error) && error == ENOENT);
    assert(!exists(root + "/other"));

    // A target that cannot be written.
    if (geteuid() != 0)
    {
        const std::string locked = root + "/locked";
        assert(mkdir(locked.c_str(), 0500) == 0);
        error = 0;
        assert(!install(app, locked + "/bun", &error) && error == EACCES);
        assert(chmod(locked.c_str(), 0700) == 0);
    }
    else
        std::puts("bun install: running as root, unwritable target case skipped");

    std::filesystem::remove_all(root);
    std::puts("PASS: bun install: streamed copy, modes, links skipped, version marker, "
              "reinstall on new version or interrupted copy, missing version, unwritable target");
}
