#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Start the staged app in an isolated X server and disposable profile."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

stage = Path(sys.argv[1]).resolve()
# xvfb-run chooses a free display and owns its authentication and cleanup.
if '--inside-xvfb' not in sys.argv:
    subprocess.run(['xvfb-run', '-a', '-s', '-screen 0 1280x800x24',
                    sys.executable, __file__, str(stage), '--inside-xvfb'], check=True)
    sys.exit(0)
with tempfile.TemporaryDirectory(prefix='scrubtub-release-smoke-') as temporary:
    profile = Path(temporary)
    runtime = profile / 'runtime'
    runtime.mkdir(mode=0o700)
    env = os.environ.copy()
    for key in ('LD_LIBRARY_PATH', 'QT_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QML_IMPORT_PATH'):
        env.pop(key, None)
    env.update(QT_QPA_PLATFORM='xcb', QT_QUICK_BACKEND='software',
               XDG_RUNTIME_DIR=str(runtime), XDG_DATA_HOME=str(profile / 'data'),
               XDG_CONFIG_HOME=str(profile / 'config'), XDG_CACHE_HOME=str(profile / 'cache'))
    with (profile / 'app.log').open('w+') as log:
        app = subprocess.Popen([str(stage / 'bin/scrubtub')], env=env, stdout=log, stderr=log)
        try:
            for attempt in range(30):
                if app.poll() is not None:
                    raise RuntimeError('Packaged application exited before showing its window')
                window = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(app.pid)],
                                        env=env, capture_output=True, text=True)
                if window.returncode == 0 and window.stdout.strip():
                    time.sleep(2)
                    if app.poll() is not None:
                        raise RuntimeError('Packaged application exited after startup')
                    print('Packaged application displayed its window successfully')
                    break
                time.sleep(1)
            else:
                raise RuntimeError('Packaged application did not display a window')
        finally:
            if app.poll() is None:
                app.terminate()
                app.wait(timeout=10)
            log.seek(0)
            print(log.read())
