"""Bun-port console harness: upload files and run probe payloads on the test PS5.

Development only. Writes on the console are limited to /data/ps5term-dev/ and
/data/ps5term/bun/ (enforced again by the recv payload). One payload at a time under the
shared TestWindow lock; every run has a 60 s host timeout after which only the probe's own
PID (announced as "PID <n>" in klog) is killed. Before every run the loader, shell and klog
servers must be alive; if one is missing the harness stops.

  PS5_HOST=<console> python3 -m tools.dev.ps5_bun_console upload LOCAL REMOTE
  PS5_HOST=<console> python3 -m tools.dev.ps5_bun_console run ELF [ARGS...]
  PS5_HOST=<console> python3 -m tools.dev.ps5_bun_console shell 'ls /data/ps5term-dev'
  PS5_HOST=<console> python3 -m tools.dev.ps5_bun_console clean REMOTE...

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations
import argparse
import hashlib
import os
import re
import socket
import sys
import threading
import time
from pathlib import Path

from .hardware_cycle import TestWindow

ELFLDR_PORT, SHSRV_PORT, KLOG_PORT, RECV_PORT = 9021, 2323, 3232, 9300
ROOT = Path(__file__).resolve().parents[2]
RECV_ELF = ROOT / 'build/bun-port/recv.elf'
LOCK = ROOT / '.local/dev/console.lock'
DEV_DIR = '/data/ps5term-dev/'
WRITABLE = (DEV_DIR, '/data/ps5term/bun/')
REQUIRED_DAEMONS = ('elfldr.elf', 'shsrv.elf', 'klogsrv.elf')
RUN_TIMEOUT = 60


class ConsoleStop(RuntimeError):
    """The console is not in a state where further tests are allowed."""


def console_addr() -> str:
    addr = os.environ.get('PS5_HOST', '').strip()
    if not addr:
        raise ConsoleStop('set PS5_HOST to the console address')
    return addr


def check_writable(remote: str) -> str:
    if (not remote.startswith(WRITABLE) or '/../' in remote or '/./' in remote or '//' in remote
            or remote.endswith(('/', '/..', '/.'))):
        raise ValueError(f'refusing console path outside {WRITABLE}: {remote}')
    return remote


class Shell:
    """shsrv telnet session; commands are restricted to read-only ones plus rm/kill."""
    PROMPT = re.compile(rb'\r?\n[^\r\n]*\$ $')

    def __init__(self):
        self.sock = socket.create_connection((console_addr(), SHSRV_PORT), 10)
        self._read_until_prompt()

    def _read_until_prompt(self, timeout=15) -> str:
        data, end = b'', time.monotonic() + timeout
        self.sock.settimeout(1)
        while time.monotonic() < end:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            data += chunk.replace(b'\xff\xfb\x01', b'').replace(b'\xff\xfb\x03', b'')
            if self.PROMPT.search(data):
                break
        return data.decode('utf-8', 'replace')

    def run(self, command: str) -> str:
        verb = command.split()[0]
        if verb not in ('ls', 'ps', 'stat', 'sum', 'rm', 'rmdir', 'kill', 'sysctl', 'procstat', 'df'):
            raise ValueError(f'shell command not allowed by the harness: {verb}')
        self.sock.sendall(command.encode() + b'\r\n')
        out = self._read_until_prompt()
        lines = out.replace('\r', '').split('\n')
        return '\n'.join(lines[:-1])  # shsrv does not echo; the last line is the prompt.

    def close(self):
        self.sock.close()


def shell(command: str) -> str:
    sh = Shell()
    try:
        return sh.run(command)
    finally:
        sh.close()


def processes() -> dict[int, str]:
    table = {}
    for line in shell('ps').splitlines():
        fields = line.split()
        if fields and fields[0].isdigit():
            table[int(fields[0])] = fields[-1]
    return table


def require_daemons() -> None:
    try:
        names = set(processes().values())
    except OSError as error:
        raise ConsoleStop(f'shsrv unreachable ({error}); stop and report') from None
    missing = [name for name in REQUIRED_DAEMONS if name not in names]
    if missing:
        raise ConsoleStop(f'console daemons missing: {missing}; stop and report')


def send_payload(path: Path, keep_open: bool = False):
    """Sends an ELF to the loader. With keep_open the socket is returned so the payload's
    stdout (which the loader may route to it) is drained instead of raising SIGPIPE."""
    data = path.read_bytes()
    if data[:4] != b'\x7fELF':
        raise ValueError(f'not an ELF: {path}')
    sock = socket.create_connection((console_addr(), ELFLDR_PORT), 10)
    sock.sendall(data)
    sock.shutdown(socket.SHUT_WR)
    if keep_open:
        return sock
    sock.close()
    return None


def upload(local: str | Path, remote: str) -> str:
    check_writable(remote)
    data = Path(local).read_bytes()
    expected = hashlib.sha256(data).hexdigest()
    loader = send_payload(RECV_ELF, keep_open=True)
    try:
        deadline = time.monotonic() + 15
        while True:
            try:
                sock = socket.create_connection((console_addr(), RECV_PORT), 3)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise ConsoleStop('recv.elf did not start listening on 9300') from None
                time.sleep(0.3)
        with sock:
            sock.settimeout(RUN_TIMEOUT)
            sock.sendall(f'{remote}\n{len(data)}\n'.encode())
            sock.sendall(data)
            answer = b''
            while not answer.endswith(b'\n'):
                chunk = sock.recv(256)
                if not chunk:
                    break
                answer += chunk
    finally:
        loader.close()
    text = answer.decode().strip()
    if text != f'OK {expected}':
        raise RuntimeError(f'upload of {remote} failed: {text!r} (expected OK {expected})')
    return expected


class Klog:
    def __init__(self):
        self.sock = socket.create_connection((console_addr(), KLOG_PORT), 10)
        self.sock.settimeout(1)
        self.buffer = b''

    def lines(self, deadline: float):
        while time.monotonic() < deadline:
            while b'\n' in self.buffer:
                line, self.buffer = self.buffer.split(b'\n', 1)
                yield line.decode('utf-8', 'replace').rstrip('\r')
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                return
            self.buffer += chunk

    def close(self):
        self.sock.close()


def _drain(sock, sink: list[str]):
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                return
            sink.append(chunk.decode('utf-8', 'replace'))
    except OSError:
        return


def run(elf: str | Path, args: list[str], name: str | None = None,
        timeout: float = RUN_TIMEOUT) -> tuple[bool, list[str], str]:
    """Runs one probe payload; returns (passed, klog lines of the probe, captured stdout)."""
    elf = Path(elf)
    name = name or elf.stem
    require_daemons()
    if any('\n' in arg for arg in args):
        raise ValueError('args are stored one per line; an argument cannot contain a newline')
    upload_args = Path(ROOT / 'build/bun-port/args.txt')
    upload_args.write_text(''.join(arg + '\n' for arg in args))
    upload(upload_args, DEV_DIR + 'args')
    klog = Klog()
    stdout: list[str] = []
    own_pids, seen, passed = [], [], None
    loader = send_payload(elf, keep_open=True)
    drain = threading.Thread(target=_drain, args=(loader, stdout), daemon=True)
    drain.start()
    try:
        for line in klog.lines(time.monotonic() + timeout):
            if f'[{name}]' not in line and f' {name}' not in line:
                continue
            seen.append(line)
            match = re.search(rf'\[{re.escape(name)}\] (?:child )?PID (\d+)', line)
            if match:
                own_pids.append(int(match.group(1)))
            if re.search(rf'\bPASS {re.escape(name)}\b', line):
                passed = True
                break
            if re.search(rf'\bFAIL {re.escape(name)}\b', line):
                passed = False
                break
    finally:
        klog.close()
    if passed is None:
        seen.append(f'TIMEOUT {name} after {timeout}s')
        table = shell('ps').splitlines()
        alive = processes()
        for pid in own_pids:
            seen.extend(f'ps: {row.strip()}' for row in table
                        if row.split() and row.split()[0] == str(pid) or
                        len(row.split()) > 1 and row.split()[1] == str(pid))
        for pid in reversed(own_pids):  # Only PIDs this test announced: child first, then probe.
            if pid in alive:
                shell(f'kill -9 {pid}')
                seen.append(f'killed own pid {pid}')
        raise ConsoleStop('\n'.join(seen))
    drain.join(5)
    loader.close()
    return passed, seen, ''.join(stdout)


LAUNCH_PORT = 9301


def launch(program: str, argv: list[str], stdin: bytes = b'',
           timeout: float = RUN_TIMEOUT) -> tuple[str, str, list[str]]:
    """Runs PROGRAM (a path on the console) through launch.elf with its stdio on a socket to the
    host (the console listens: hosts often firewall inbound connections).
    Returns (status from klog, e.g. "exit=0", program output, klog lines)."""
    output: list[str] = []
    stop = threading.Event()

    def attach():
        deadline = time.monotonic() + 40
        while not stop.is_set() and time.monotonic() < deadline:
            try:
                conn = socket.create_connection((console_addr(), LAUNCH_PORT), 2)
            except OSError:
                time.sleep(0.3)
                continue
            with conn:
                conn.settimeout(None)
                if stdin:
                    conn.sendall(stdin)
                _drain(conn, output)
            return

    reader = threading.Thread(target=attach, daemon=True)
    reader.start()
    try:
        passed, lines, _ = run(ROOT / 'build/bun-port/launch.elf',
                               [str(LAUNCH_PORT), program, *argv], 'launch', timeout)
    except ConsoleStop as error:
        reader.join(3)
        raise ConsoleStop(f'{error}\n--- program output so far ---\n{"".join(output)}') from None
    finally:
        stop.set()
        reader.join(10)
    status = next((m.group(1) for line in lines
                   if (m := re.search(r'PASS launch (\S+)', line))), 'failed')
    return status if passed else 'failed', ''.join(output), lines


def clean(paths: list[str]) -> None:
    for remote in paths:
        if not remote.startswith(DEV_DIR):
            raise ValueError(f'clean is limited to {DEV_DIR}: {remote}')
        check_writable(remote)
        shell(f'rm {remote}')


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('upload'); p.add_argument('local'); p.add_argument('remote')
    p = sub.add_parser('run'); p.add_argument('elf'); p.add_argument('args', nargs='*')
    p = sub.add_parser('launch'); p.add_argument('program'); p.add_argument('args', nargs='*')
    p.add_argument('--timeout', type=float, default=RUN_TIMEOUT)
    p = sub.add_parser('shell'); p.add_argument('line')
    p = sub.add_parser('clean'); p.add_argument('paths', nargs='+')
    options = parser.parse_args(argv)
    with TestWindow(LOCK):
        if options.command == 'upload':
            print('OK', upload(options.local, options.remote))
        elif options.command == 'run':
            passed, lines, stdout = run(options.elf, options.args)
            print('\n'.join(lines))
            if stdout:
                print('--- stdout ---\n' + stdout)
            return 0 if passed else 1
        elif options.command == 'launch':
            status, output, lines = launch(options.program, options.args, timeout=options.timeout)
            sys.stdout.write(output)
            print(f'--- {status}', file=sys.stderr)
            if status == 'failed':
                print('\n'.join(lines), file=sys.stderr)
            return 0 if status == 'exit=0' else 1
        elif options.command == 'shell':
            print(shell(options.line))
        else:
            clean(options.paths)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except ConsoleStop as stop:
        print(f'CONSOLE STOP: {stop}', file=sys.stderr)
        sys.exit(3)
