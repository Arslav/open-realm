"""Pinned ANGLE imports must never silently accept another ABI, hash, or legacy backend."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('android_angle', Path(__file__).resolve().parents[1] / 'tools/android/fetch_angle.py')
angle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(angle)


class AngleImportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='openrealm-angle-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        # A synthetic ARM64 ELF identity keeps the test independent of SDK/retail files.
        self.data = b'\x7fELF\x02\x01' + bytes(12) + (183).to_bytes(2, 'little')
        self.files = {name: hashlib.sha256(self.data + bytes([i])).hexdigest()
                      for i, name in enumerate(angle.FILES)}
        for i, name in enumerate(self.files):
            (self.source / name).write_bytes(self.data + bytes([i]))
        self.addCleanup(patch.stopall)
        patch.object(angle, 'FILES', self.files).start()
        self.network = patch.object(angle.urllib.request, 'urlopen', side_effect=AssertionError('unexpected download')).start()

    def test_verified_import_and_cached_reuse(self):
        dest = angle.fetch(self.root / 'cache', self.source)
        manifest = json.loads((dest / 'manifest.json').read_text())
        self.assertEqual(manifest['flavor'], 'sdk35')
        self.assertEqual({x['sha256'] for x in manifest['files']}, set(self.files.values()))
        self.assertEqual(angle.fetch(dest), dest)
        self.network.assert_not_called()

    def test_missing_modern_cache_does_not_fall_back_to_legacy(self):
        with self.assertRaisesRegex(RuntimeError, 'extract_angle.py'):
            angle.fetch(self.root / 'missing')
        self.network.assert_not_called()

    def test_corrupt_cache_is_rejected_and_preserved(self):
        dest = angle.fetch(self.root / 'cache', self.source)
        name = next(iter(self.files))
        (dest / name).write_bytes(b'corrupt cache')
        with self.assertRaisesRegex(RuntimeError, 'hash mismatch'):
            angle.fetch(dest)
        self.assertEqual((dest / name).read_bytes(), b'corrupt cache')

    def test_invalid_source_does_not_partially_replace_verified_cache(self):
        dest = angle.fetch(self.root / 'cache', self.source)
        before = {p.name: p.read_bytes() for p in dest.iterdir()}
        (self.source / list(self.files)[1]).write_bytes(b'bad second library')
        with self.assertRaisesRegex(RuntimeError, 'hash mismatch'):
            angle.fetch(dest, self.source)
        self.assertEqual({p.name: p.read_bytes() for p in dest.iterdir()}, before)

    def test_wrong_abi_is_rejected_even_when_hash_matches(self):
        x86 = self.data[:18] + (62).to_bytes(2, 'little')
        with self.assertRaisesRegex(RuntimeError, 'ARM64 ELF'):
            angle.verify(x86, 'wrong-abi.so', hashlib.sha256(x86).hexdigest())


if __name__ == '__main__':
    unittest.main()
