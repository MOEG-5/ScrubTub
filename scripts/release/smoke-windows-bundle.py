#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate the staged GUI on a disposable Windows CI runner, with no MSYS2 PATH."""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true':
    raise RuntimeError('GUI smoke test requires the dedicated Windows Actions runner')
stage = Path(sys.argv[1]).resolve()
user32 = ctypes.WinDLL('user32', use_last_error=True)
callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
with tempfile.TemporaryDirectory(prefix='scrubtub-release-smoke-', ignore_cleanup_errors=True) as temporary:
    profile = Path(temporary)
    env = os.environ.copy()
    for key in ('QT_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QML_IMPORT_PATH'):
        env.pop(key, None)
    env.update(PATH=str(Path(os.environ['SystemRoot']) / 'System32'),
               QT_QPA_PLATFORM='windows', QT_QUICK_BACKEND='software',
               APPDATA=str(profile / 'roaming'), LOCALAPPDATA=str(profile / 'local'))
    with (profile / 'app.log').open('w+') as log:
        app = subprocess.Popen([str(stage / 'bin/scrubtub.exe')], env=env, stdout=log, stderr=log)
        visible = []
        @callback_type
        def inspect(window, unused):
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
            if owner.value == app.pid and user32.IsWindowVisible(window):
                visible.append(window)
            return True
        try:
            for attempt in range(30):
                if app.poll() is not None:
                    raise RuntimeError('Packaged application exited before showing a window')
                user32.EnumWindows(inspect, 0)
                if visible:
                    time.sleep(2)
                    if app.poll() is not None:
                        raise RuntimeError('Packaged application exited after startup')
                    print('Packaged Windows application displayed its window with a clean PATH')
                    break
                time.sleep(1)
            else:
                raise RuntimeError('Packaged application did not display a window')
        finally:
            if app.poll() is None:
                for window in visible:
                    user32.PostMessageW(window, 0x0010, 0, 0)  # WM_CLOSE: allow child-process cleanup
                try:
                    app.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    subprocess.run([str(Path(os.environ['SystemRoot']) / 'System32/taskkill.exe'),
                                    '/PID', str(app.pid), '/T', '/F'], check=True, capture_output=True)
                    app.wait(timeout=10)
            log.seek(0)
            print(log.read())
