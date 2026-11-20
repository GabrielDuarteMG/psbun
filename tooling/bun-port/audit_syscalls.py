#!/usr/bin/env python3
"""Audit the raw system calls a PS5 payload can make (PS5Term Bun port).

The PS5 kernel numbers its own (Sony) system calls from 532 upward, so a FreeBSD 12+ system call
number compiled into a payload can reach an unrelated Sony call instead. This audit
disassembles the ELF and fails unless every system call number it can see is in
tooling/bun-port/allowed_syscalls.json (the FreeBSD 11 numbers that the PS5 payload SDK's own libc
stubs use, plus the documented exceptions inherited from shsrv's elfldr_spawn):

  * an inline `syscall` instruction must have its number loaded into %eax/%rax by an immediate
    in the same function, unless the function is one of the SDK's generic syscall(2) wrappers;
  * a call to syscall()/__syscall() (direct, or through the GOT with -fno-plt) must pass an
    immediate number in %edi/%rdi.

  python3 tooling/bun-port/audit_syscalls.py build/bun-port/bun.elf
  python3 tooling/bun-port/audit_syscalls.py --collect SDK/target/lib/libc.a SDK/target/lib/crt1.o

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ALLOWED = HERE / 'allowed_syscalls.json'
FIRST_SONY_RANGE = 551  # Anything at or above this is outside the FreeBSD 11 table.
GENERIC_WRAPPERS = {'syscall', '__syscall'}
WINDOW = 16

FUNCTION = re.compile(r'^[0-9a-f]+ <(?P<name>[^>]+)>:$')
INSTRUCTION = re.compile(r'^\s*(?P<addr>[0-9a-f]+):\s+(?P<text>.+?)\s*$')
IMM_TO_AX = re.compile(r'^mov[lq]?\s+\$(?P<imm>0x[0-9a-f]+|\d+),\s*%(?:e|r)ax\b')
WRITES_AX = re.compile(r'%(?:e|r)ax$|%ax$|%al$')
IMM_TO_DI = re.compile(r'^mov[lq]?\s+\$(?P<imm>0x[0-9a-f]+|\d+),\s*%(?:e|r)di\b')
WRITES_DI = re.compile(r'%(?:e|r)di$|%di$|%dil$')
DIRECT_CALL = re.compile(r'^call[q]?\s+(?:0x)?[0-9a-f]+\s+<(?P<target>[^>@+]+)')
GOT_CALL = re.compile(r'^call[q]?\s+\*0x[0-9a-f]+\(%rip\)\s+#\s+0x(?P<slot>[0-9a-f]+)')


def run(*command: str) -> str:
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def objdump(path: str) -> str:
    return run('llvm-objdump', '-d', '--no-show-raw-insn', '-M', 'att', path)


def got_slots(path: str) -> dict[int, str]:
    """GOT slot address -> imported symbol, from the dynamic relocations."""
    slots = {}
    try:
        text = run('llvm-readelf', '-r', '--wide', path)
    except subprocess.CalledProcessError:
        return slots
    for line in text.splitlines():
        fields = line.split()
        if (len(fields) >= 5 and re.fullmatch(r'[0-9a-f]{8,16}', fields[0])
                and any(kind in fields[2] for kind in ('GLOB_DAT', 'JUMP_SLOT'))):
            slots[int(fields[0], 16)] = fields[4].split('@')[0]
    return slots


def scan(path: str):
    """Yields (kind, function, address, number-or-None) for every system call site."""
    slots = {addr: name for addr, name in got_slots(path).items() if name in GENERIC_WRAPPERS}
    function, window = '?', []
    for line in objdump(path).splitlines():
        match = FUNCTION.match(line)
        if match:
            function, window = match.group('name'), []
            continue
        match = INSTRUCTION.match(line)
        if not match:
            continue
        address, text = match.group('addr'), match.group('text')
        text = re.sub(r'\s+', ' ', text)
        text = re.sub(r'^(?:(?:addr32|data16|notrack|bnd|rex64|lock|rep|repne) )+', '', text)
        if text == 'syscall':
            yield 'inline', function, address, last_immediate(window, IMM_TO_AX, WRITES_AX)
        else:
            call = DIRECT_CALL.match(text)
            target = call.group('target') if call else None
            got = GOT_CALL.match(text)
            if got and int(got.group('slot'), 16) in slots:
                target = slots[int(got.group('slot'), 16)]
            if target in GENERIC_WRAPPERS and function not in GENERIC_WRAPPERS:
                yield 'call', function, address, last_immediate(window, IMM_TO_DI, WRITES_DI)
        window.append(text)
        del window[:-WINDOW]


def last_immediate(window: list[str], immediate: re.Pattern, writes: re.Pattern):
    """The immediate last loaded into the register, or None if it was computed (AT&T syntax:
    the destination is the last operand)."""
    for text in reversed(window):
        match = immediate.match(text)
        if match:
            return int(match.group('imm'), 0)
        if text.startswith(('call', 'jmp', 'ret')):
            return None
        operands = text.split(' ', 1)[1] if ' ' in text else ''
        if writes.search(operands):
            return None
    return None


def collect(paths: list[str]) -> int:
    numbers = set()
    for path in paths:
        for kind, function, address, number in scan(path):
            if number is not None:
                numbers.add(number)
    print(json.dumps(sorted(numbers)))
    return 0


def audit(path: str) -> int:
    allowed = json.loads(ALLOWED.read_text())
    table = set(allowed['freebsd11_sdk_libc'])
    exceptions = {int(k): v for k, v in allowed['inherited_exceptions'].items()}
    wrappers = set(allowed.get('generic_wrapper_functions', [])) | GENERIC_WRAPPERS
    failures, sites = [], 0
    for kind, function, address, number in scan(path):
        sites += 1
        where = f'{kind} system call in {function} at 0x{address}'
        if number is None:
            if kind == 'inline' and function in wrappers:
                continue  # The SDK's syscall(2) itself; its callers are audited as "call" sites.
            failures.append(f'{where}: number is not an immediate')
        elif number in exceptions:
            if function not in exceptions[number]['functions']:
                failures.append(f'{where}: {number} is only allowed in {exceptions[number]["functions"]}')
        elif number >= FIRST_SONY_RANGE or number not in table:
            failures.append(f'{where}: {number} is not in the FreeBSD 11 SDK table')
    for failure in failures:
        print('FAIL', failure)
    print(f'audit_syscalls: {sites} system call sites, {len(failures)} failures')
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--collect', action='store_true', help='print the numbers used by PATHS')
    parser.add_argument('paths', nargs='+')
    options = parser.parse_args()
    if options.collect:
        return collect(options.paths)
    return max(audit(path) for path in options.paths)


if __name__ == '__main__':
    sys.exit(main())
