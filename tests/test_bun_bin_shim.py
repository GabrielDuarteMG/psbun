# SPDX-License-Identifier: GPL-3.0-or-later
"""Bun bin shims (tooling/bun-port/ps5compat/binshim.c): the file Bun writes where /data has no symlinks."""
from pathlib import Path
import ctypes
import errno
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPAT = ROOT/'tooling/bun-port/ps5compat'
AT_FDCWD = -100


def shim_bytes(target):
    return (b'#!/usr/bin/env bun\n// bun-ps5-bin-shim ' + target + b'\n'
            b'require(require("path").resolve(__dirname, "' + target + b'"));\n')


@unittest.skipUnless(shutil.which('cc'), 'needs a host C compiler')
class BunBinShimTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._build = tempfile.TemporaryDirectory()
        lib = os.path.join(cls._build.name, 'libbinshim.so')
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Werror', '-I'+str(COMPAT), '-o', lib,
                        str(COMPAT/'binshim.c')], check=True)
        c = ctypes.CDLL(lib, use_errno=True)
        c.ps5compat_bin_shim_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        c.ps5compat_bin_shim_create.restype = ctypes.c_int
        c.ps5compat_bin_shim_read.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        c.ps5compat_bin_shim_read.restype = ctypes.c_ssize_t
        c.ps5compat_bin_shim_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_char_p)]
        c.ps5compat_bin_shim_parse.restype = ctypes.c_ssize_t
        c.ps5compat_bin_shim_resolve.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
        c.ps5compat_bin_shim_resolve.restype = ctypes.c_int
        cls.c = c

    @classmethod
    def tearDownClass(cls):
        cls._build.cleanup()

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.d = self._tmp.name
        os.mkdir(os.path.join(self.d, '.bin'))
        self.shim = os.path.join(self.d, '.bin', 'x').encode()

    def create(self, path, target):
        ctypes.set_errno(0)
        result = self.c.ps5compat_bin_shim_create(path, target)
        return result, ctypes.get_errno()

    def read(self, dirfd, path, size=1024):
        buf = ctypes.create_string_buffer(size)
        ctypes.set_errno(0)
        n = self.c.ps5compat_bin_shim_read(dirfd, path, buf, size)
        return n, ctypes.get_errno(), buf.value

    def resolve(self, shim, target):
        out = ctypes.create_string_buffer(os.pathconf('/', 'PC_PATH_MAX') + 1)
        error = self.c.ps5compat_bin_shim_resolve(shim, target, len(target), out)
        return error, out.value

    def test_create_writes_exact_format_with_0755_despite_umask(self):
        old = os.umask(0o077)
        try:
            self.assertEqual(self.create(self.shim, b'../pkg/cli.js')[0], 0)
        finally:
            os.umask(old)
        self.assertEqual(Path(self.shim.decode()).read_bytes(), shim_bytes(b'../pkg/cli.js'))
        self.assertEqual(os.stat(self.shim).st_mode & 0o777, 0o755)

    def test_create_refuses_existing_path(self):
        Path(self.shim.decode()).write_bytes(b'mine')
        self.assertEqual(self.create(self.shim, b'../pkg/cli.js'), (-1, errno.EEXIST))
        self.assertEqual(Path(self.shim.decode()).read_bytes(), b'mine')

    def test_create_rejects_invalid_targets(self):
        for target in (b'a"b', b'a\\b', b'a\nb', b'a\rb', b'', b'x'*1024):
            with self.subTest(target=target):
                self.assertEqual(self.create(self.shim, target), (-1, errno.EINVAL))
                self.assertFalse(os.path.lexists(self.shim))

    def test_read_returns_target_by_path_and_dirfd(self):
        self.assertEqual(self.create(self.shim, b'../pkg/cli.js')[0], 0)
        n, _, value = self.read(AT_FDCWD, self.shim)
        self.assertEqual((n, value), (13, b'../pkg/cli.js'))
        fd = os.open(os.path.join(self.d, '.bin'), os.O_RDONLY)
        try:
            n, _, value = self.read(fd, b'x')
        finally:
            os.close(fd)
        self.assertEqual((n, value), (13, b'../pkg/cli.js'))

    def test_read_rejects_non_shims(self):
        Path(self.shim.decode()).write_bytes(b'hello\n')
        self.assertEqual(self.read(AT_FDCWD, self.shim)[:2], (-1, errno.EINVAL))
        self.assertEqual(self.read(AT_FDCWD, os.path.join(self.d, '.bin').encode())[:2], (-1, errno.EINVAL))
        self.assertEqual(self.read(AT_FDCWD, os.path.join(self.d, 'missing').encode())[:2], (-1, errno.ENOENT))

    def test_read_reports_small_buffer(self):
        self.assertEqual(self.create(self.shim, b'../pkg/cli.js')[0], 0)
        for size in (13, 5):
            with self.subTest(size=size):
                self.assertEqual(self.read(AT_FDCWD, self.shim, size)[:2], (-1, errno.ENAMETOOLONG))

    def test_parse_rejects_truncated_and_foreign_headers(self):
        target = ctypes.c_char_p()
        full = shim_bytes(b'../pkg/cli.js')
        self.assertEqual(self.c.ps5compat_bin_shim_parse(full, len(full), ctypes.byref(target)), 13)
        truncated = b'#!/usr/bin/env bun\n// bun-ps5-bin-shim ../pkg/cli.js'
        self.assertEqual(self.c.ps5compat_bin_shim_parse(truncated, len(truncated), ctypes.byref(target)), -1)
        node = b'#!/usr/bin/env node\n// bun-ps5-bin-shim x\n'
        self.assertEqual(self.c.ps5compat_bin_shim_parse(node, len(node), ctypes.byref(target)), -1)

    def test_resolve_joins_shim_directory_and_target(self):
        pkg = Path(self.d)/'pkg'
        pkg.mkdir()
        (pkg/'cli.js').write_text('')
        self.assertEqual(self.resolve(self.shim, b'../pkg/cli.js'),
                         (0, os.path.realpath(pkg/'cli.js').encode()))
        self.assertEqual(self.resolve(self.shim, b'../pkg/missing.js')[0], errno.ENOENT)
        cwd = os.getcwd()
        os.chdir(self.d)
        try:
            error, out = self.resolve(b'.bin/x', b'../pkg/cli.js')
        finally:
            os.chdir(cwd)
        self.assertEqual((error, out), (0, os.path.realpath(pkg/'cli.js').encode()))


if __name__ == '__main__': unittest.main()
