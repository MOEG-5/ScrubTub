# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('verify_release', Path(__file__).parents[1] / 'verify-release.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReleaseGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.stage = Path(self.temp.name) / 'stage'
        self.sources = Path(self.temp.name) / 'sources'
        self.stage.mkdir()
        self.sources.mkdir()
        self.write(self.stage / 'bin/tool', b'runtime')
        self.write(self.sources / 'dependency-source.tar', b'corresponding-source')
        for name in ('LICENSE', 'THIRD_PARTY.md', 'DEPENDENCIES.md', 'licenses/dependency/LICENSE'):
            self.write(self.stage / 'share/scrubtub' / name, b'license and notices')
        app = {'CMakeLists.txt': b'build rules', 'LICENSE': b'app license'}
        for name, value in app.items():
            self.write(self.sources / 'scrubtub' / name, value)
        self.write(self.sources / 'application-source.json', json.dumps({k: self.sha(v) for k, v in app.items()}).encode())
        manifest = {
            'format': 1,
            'components': {'dependency': {'source_path': 'dependency-source.tar', 'notice_path': 'share/scrubtub/licenses/dependency'}},
            'source_files': {'dependency-source.tar': self.sha(b'corresponding-source')},
            'notice_files': {'share/scrubtub/licenses/dependency/LICENSE': self.sha(b'license and notices')},
            'files': [{'file': 'bin/tool', 'component': 'dependency', 'sha256': self.sha(b'runtime')}],
        }
        self.write(self.stage / 'share/scrubtub/DEPENDENCIES.json', json.dumps(manifest).encode())

    @staticmethod
    def sha(value):
        return hashlib.sha256(value).hexdigest()

    @staticmethod
    def write(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)

    def verify(self):
        with contextlib.redirect_stdout(io.StringIO()):
            module.verify(self.stage, self.sources)

    def test_complete_artifact_passes(self):
        self.verify()

    def test_missing_corresponding_source_fails(self):
        (self.sources / 'dependency-source.tar').unlink()
        with self.assertRaisesRegex(ValueError, 'dependency source'):
            self.verify()

    def test_changed_binary_fails(self):
        (self.stage / 'bin/tool').write_bytes(b'different build')
        with self.assertRaisesRegex(ValueError, 'runtime'):
            self.verify()

    def test_stripped_copyright_notice_fails(self):
        (self.stage / 'share/scrubtub/licenses/dependency/LICENSE').write_bytes(b'')
        with self.assertRaisesRegex(ValueError, 'notice'):
            self.verify()

    def test_extra_library_fails(self):
        self.write(self.stage / 'bin/unaccounted.dll', b'MZbinary')
        with self.assertRaisesRegex(ValueError, 'Uninventoried'):
            self.verify()

    def test_executable_in_notice_directory_fails(self):
        self.write(self.stage / 'share/scrubtub/hidden', b'\x7fELFbinary')
        with self.assertRaisesRegex(ValueError, 'hidden in notices'):
            self.verify()

    def test_changed_application_source_fails(self):
        (self.sources / 'scrubtub/CMakeLists.txt').write_text('unrelated build rules')
        with self.assertRaisesRegex(ValueError, 'application source'):
            self.verify()


if __name__ == '__main__':
    unittest.main()
