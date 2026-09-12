"""Offline bundle/preflight tests. No Hyper-V objects or network changes."""
import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('prepare_bundle', HERE/'prepare-bundle.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nexus-desktop-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root/'clean.vhdx'
        # Format marker for packaging tests only, deliberately not a bootable disk.
        self.image.write_bytes(b'vhdxfile' + b'synthetic-test-image')
        self.output = self.root/'bundle'

    def prepare(self):
        module.prepare(self.image, self.output)

    def check(self):
        shell = shutil.which('pwsh') or shutil.which('powershell')
        if not shell:
            self.skipTest('PowerShell is unavailable')
        return subprocess.run([shell, '-NoProfile', '-NonInteractive', '-File',
                               str(self.output/'start-nexus-openwrt.ps1'), '-Action', 'Check',
                               '-BundleDirectory', str(self.output), '-InstallationDirectory', str(self.root/'installation')],
                              capture_output=True, timeout=30)

    def test_valid_preflight_has_no_installation_side_effect(self):
        self.prepare()
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root/'installation').exists())

    def test_changed_image_rejected(self):
        self.prepare()
        (self.output/'nexus-openwrt-desktop.vhdx').write_bytes(b'changed')
        self.assertNotEqual(self.check().returncode, 0)

    def test_path_escape_rejected(self):
        self.prepare()
        path = self.output/'desktop-image.json'
        data = json.loads(path.read_text())
        data['file'] = '../clean.vhdx'
        path.write_text(json.dumps(data))
        self.assertNotEqual(self.check().returncode, 0)

    def test_wrong_network_profile_rejected(self):
        self.prepare()
        path = self.output/'desktop-image.json'
        data = json.loads(path.read_text())
        data['lan_address'] = '192.168.1.1'
        path.write_text(json.dumps(data))
        self.assertNotEqual(self.check().returncode, 0)

    def test_existing_output_not_overwritten(self):
        self.prepare()
        with self.assertRaises(FileExistsError):
            self.prepare()

    def test_wrong_ipv6_profile_rejected(self):
        self.prepare()
        path = self.output/'desktop-image.json'
        data = json.loads(path.read_text())
        data['lan_ipv6'] = '2001:db8::1'
        path.write_text(json.dumps(data))
        self.assertNotEqual(self.check().returncode, 0)

    def test_invalid_vhdx_rejected(self):
        self.image.write_bytes(b'not-a-vhdx')
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
