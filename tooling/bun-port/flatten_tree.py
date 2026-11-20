#!/usr/bin/env python3
"""Turn a `bun install -g` tree into the plain-file layout shipped in assets/bun/install.

The PS5Term app folder and its ZIP carry no symlinks or exec bits, and payloads cannot use
them anyway, so every symlink is replaced by a copy of its target. Also:
  * drops other platforms' @oh-my-pi/pi-natives-* packages and bun's install cache;
  * writes <pi-natives>/native/pi_natives.freebsd-x64-baseline.node, a 64-byte marker that only
    carries the addon release stamp (the addon itself is linked into bun.elf; ps5compat's
    dlopen() maps this path to it);
  * checks that the omp entry point exists.

  python3 tooling/bun-port/flatten_tree.py BUN_INSTALL_ROOT OUTPUT_DIR VERSION

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations
import os
import shutil
import sys
from pathlib import Path

ADDON = 'pi_natives.freebsd-x64-baseline.node'
STAMP_MAGIC = b'PI_NATIVES_VERSION_STAMP:'
STAMP_SIZE = 64
CLI = 'node_modules/@oh-my-pi/pi-coding-agent/dist/cli.js'


def copy_resolved(source: Path, target: Path, root: Path) -> None:
    """Copies SOURCE to TARGET following symlinks that stay inside ROOT."""
    if source.is_symlink():
        resolved = source.resolve()
        if root not in resolved.parents and resolved != root:
            raise SystemExit(f'symlink leaves the install tree: {source} -> {resolved}')
        source = resolved
    if source.is_dir():
        target.mkdir(parents=True, exist_ok=True)
        for child in sorted(source.iterdir()):
            copy_resolved(child, target / child.name, root)
    elif source.is_file():
        shutil.copyfile(source, target)
        os.chmod(target, 0o644)
    else:
        raise SystemExit(f'not a regular file or directory: {source}')


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    install_root, output, version = Path(sys.argv[1]).resolve(), Path(sys.argv[2]), sys.argv[3]
    global_dir = install_root / 'install' / 'global'
    if not (global_dir / CLI).is_file():
        raise SystemExit(f'{global_dir / CLI} missing: run bun install -g first')
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    for name in ('node_modules', 'package.json'):
        copy_resolved(global_dir / name, output / name, install_root)
    scope = output / 'node_modules' / '@oh-my-pi'
    for package in scope.glob('pi-natives-*'):
        shutil.rmtree(package)
    native = scope / 'pi-natives' / 'native'
    if not native.is_dir():
        raise SystemExit(f'{native} missing')
    stamp = STAMP_MAGIC + version.encode() + b'\0'
    (native / ADDON).write_bytes(stamp.ljust(STAMP_SIZE, b'\0'))
    links = [p for p in output.rglob('*') if p.is_symlink()]
    if links:
        raise SystemExit(f'symlinks left: {links[:5]}')
    files = sum(1 for p in output.rglob('*') if p.is_file())
    print(f'flatten_tree: {files} files in {output}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
