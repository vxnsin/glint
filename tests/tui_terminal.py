#!/usr/bin/env python3
"""Exercise the real terminal UI with a fake daemon, without Wayland or sockets."""
import fcntl
import base64
import json
import os
import pathlib
import pty
import re
import select
import signal
import struct
import sys
import tempfile
import termios
import time
import zlib


def png(path):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = b''.join(b'\0' + bytes([40, 100 + y * 3, 180]) * 32 for y in range(32))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', 32, 32, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


with tempfile.TemporaryDirectory(prefix='glint-tui-test-') as directory:
    root = pathlib.Path(directory)
    item = root / 'workshop' / '1234'
    item.mkdir(parents=True)
    (item / 'project.json').write_text(json.dumps({'title': 'Röxy test', 'type': 'scene', 'file': 'scene.json', 'preview': 'preview.png'}))
    png(item / 'preview.png')
    requests = root / 'requests.jsonl'
    tools = root / 'bin'
    tools.mkdir()
    steamcmd = tools / 'steamcmd'
    steamcmd.write_text('''#!/usr/bin/python3
import json, os, pathlib, sys
args = sys.argv[1:]
assert args[0] == '+force_install_dir' and args[2:6] == ['+login', 'demo', '+workshop_download_item', '431960'], args
assert args[6:] == ['5678', '+quit'], args
pathlib.Path(os.environ['GLINT_TEST_DOWNLOAD']).write_text(json.dumps(args))
folder = pathlib.Path(args[1]) / 'steamapps/workshop/content/431960/5678'
folder.mkdir(parents=True)
(folder / 'project.json').write_text(json.dumps({'title':'Downloaded test', 'type':'video', 'file':'movie.mp4'}))
(folder / 'movie.mp4').touch()
print('Test SteamCMD download complete')
''')
    steamcmd.chmod(0o755)
    pid, terminal = pty.fork()
    if pid == 0:
        os.environ.pop('NO_COLOR', None)
        os.environ['GLINT_TUI_IMAGES'] = 'symbols'
        os.environ.update(TERM='xterm-256color', LANG='C.UTF-8', GLINT_WORKSHOP=str(item.parent), GLINT_TEST_REQUESTS=str(requests))
        os.environ.update(XDG_DATA_HOME=str(root / 'data'), GLINT_TEST_DOWNLOAD=str(root / 'download.json'), PATH=str(tools) + ':' + os.environ['PATH'])
        os.execv(sys.argv[1], [sys.argv[1]])
    fcntl.ioctl(terminal, termios.TIOCSWINSZ, struct.pack('HHHH', 34, 120, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    screen = bytearray()
    exited = False

    def pump(duration=0.15):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            if select.select([terminal], [], [], max(0, end - time.monotonic()))[0]:
                try:
                    part = os.read(terminal, 65536)
                except OSError:
                    return
                if not part:
                    return
                screen.extend(part)

    def logged():
        if not requests.exists():
            return []
        return [json.loads(line) for line in requests.read_text().splitlines() if line]

    def wait(predicate, label, timeout=4):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            pump()
            if predicate():
                return
        raise AssertionError(label + '\nRequests: ' + repr(logged()) + '\nTerminal tail: ' + repr(bytes(screen[-1800:])))

    def send(keys):
        os.write(terminal, keys)
        pump(0.2)

    try:
        wait(lambda: any(r['op'] == 'status' for r in logged()), 'initial daemon state missing')
        wait(lambda: '▀'.encode() in screen, 'image preview did not render')
        send(b'\n')
        wait(lambda: any(r.get('path') == str(item) for r in logged()), 'Enter did not apply selected wallpaper')
        wait(lambda: len([r for r in logged() if r['op'] == 'status']) >= 2, 'state did not refresh after apply')
        send(b'2\n')
        assert b'Global playback' in screen, 'global settings scope missing'
        send(b'\n')
        for _ in range(6):
            send(b'\x1b[B')
        send(b'\n')
        wait(lambda: any(r['op'] == 'settings' and r.get('set', {}).get('fps') == 144 for r in logged()), 'global FPS picker failed')
        assert not any(r.get('options', {}).get('fps') == 144 for r in logged()), 'global setting used per-wallpaper replacement'
        pump(0.3)
        send(b'o')
        send(b'\x1b[B\n')
        wait(lambda: any(r['op'] == 'props' for r in logged()), 'individual display selection failed')
        for _ in range(4):
            send(b'\x1b[B')
        send(b'\n')
        send(b'\x7f\x7f25\n')
        wait(lambda: any(r['op'] == 'settings' and r.get('set', {}).get('volume') == 0.25 for r in logged()), 'global volume percent conversion failed')
        pump(0.3)
        for _ in range(3):
            send(b'\x1b[B')
        send(b'\n')
        wait(lambda: any(r.get('set', {}).get('clock') is False for r in logged()), 'wallpaper property toggle failed')
        send(b'4')
        send(b'\x1b[B\x1b[B\n')
        send(b'5678\n')
        send(b'demo\n')
        wait(lambda: (root / 'download.json').exists(), 'SteamCMD was not started with the download request')
        wait(lambda: b'Press Enter to return' in screen, 'SteamCMD did not return to glint')
        send(b'\n')
        wait(lambda: (root / 'data/glint/workshop/431960/5678/project.json').exists(), 'completed download was not registered')
        send(b'1/')
        before = len(screen)
        pump(0.5)
        assert len(screen) - before < 128, 'search dialog redraws idle frames and may flicker'
        send('Röxy'.encode() + b'\n')
        fcntl.ioctl(terminal, termios.TIOCSWINSZ, struct.pack('HHHH', 15, 55, 0, 0))
        os.kill(pid, signal.SIGWINCH)
        wait(lambda: b'resize the terminal' in screen, 'resize handling failed')
        send(b'q')
        _, status = os.waitpid(pid, 0)
        exited = True
        assert os.waitstatus_to_exitcode(status) == 0, status
        flags = termios.tcgetattr(terminal)[3]
        assert flags & termios.ICANON and flags & termios.ECHO, 'terminal input mode was not restored'
        print('TUI terminal: arrows, image preview, FPS, volume, properties, SteamCMD handoff/import, Unicode search, resize and cleanup passed')
    finally:
        if not exited:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        os.close(terminal)

    pid, terminal = pty.fork()
    if pid == 0:
        os.environ.pop('NO_COLOR', None)
        os.environ.update(TERM='xterm-256color', LANG='C.UTF-8', GLINT_TUI_IMAGES='kitty', GLINT_WORKSHOP=str(item.parent), GLINT_TEST_REQUESTS=str(requests), XDG_DATA_HOME=str(root / 'kitty-data'))
        os.execv(sys.argv[1], [sys.argv[1]])
    fcntl.ioctl(terminal, termios.TIOCSWINSZ, struct.pack('HHHH', 34, 120, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    screen = bytearray()
    exited = False
    try:
        wait(lambda: b'\x1b_Gm=0;' in screen, 'Kitty preview was not transmitted')
        commands = re.findall(rb'\x1b_G([^;]*);([A-Za-z0-9+/=]*)\x1b\\', screen)
        assert commands and commands[0][0].startswith(b'a=T,f=24,t=d,q=2,C=1,'), commands[:1]
        metadata = dict(part.split(b'=') for part in commands[0][0].split(b','))
        payload = b''.join(chunk for _, chunk in commands)
        assert len(base64.b64decode(payload)) == int(metadata[b's']) * int(metadata[b'v']) * 3
        assert all(len(chunk) <= 4096 for _, chunk in commands)
        send(b'\t')
        wait(lambda: b'\x1b_Ga=d,d=I,' in screen, 'Kitty preview was not removed when leaving the library')
        send(b'q')
        _, status = os.waitpid(pid, 0)
        exited = True
        assert os.waitstatus_to_exitcode(status) == 0
        print('TUI Kitty graphics: RGB payload, chunk limits and image cleanup passed')
    finally:
        if not exited:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        os.close(terminal)
