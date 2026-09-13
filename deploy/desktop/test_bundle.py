"""Offline bundle/preflight tests. No Hyper-V objects or network changes."""
import importlib.util
import json
import shutil
import os
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
        shell = shutil.which('powershell') or shutil.which('pwsh')
        if not shell:
            self.skipTest('PowerShell is unavailable')
        env = os.environ.copy()
        if Path(shell).name.lower() == 'powershell.exe':
            # Python launched from pwsh otherwise leaks PowerShell 7 module paths
            # into Windows PowerShell 5.1; let it use its own system modules.
            for name in list(env):
                if name.lower() == 'psmodulepath':
                    del env[name]
        return subprocess.run([shell, '-NoProfile', '-NonInteractive', '-File',
                               str(self.output/'start-nexus-openwrt.ps1'), '-Action', 'Check',
                               '-InstallationDirectory', str(self.root/'installation')],
                              capture_output=True, timeout=30, env=env)

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

    def test_ipv6_tracks_hyperv_child_directory(self):
        shell = shutil.which('pwsh') or shutil.which('powershell')
        if not shell:
            self.skipTest('PowerShell is unavailable')
        installation = self.root/'installation'
        installation.mkdir()
        state = dict(installation=str(installation), configured=True,
                     vm_id='00000000-0000-0000-0000-000000000001',
                     switch_id='00000000-0000-0000-0000-000000000002',
                     vm_name='Nexus-OpenWrt-Desktop')
        wrapper = self.root/'ipv6-test.ps1'
        wrapper.write_text('''param($Script, $Installation, $Corrupt)
$global:desktopTestVmPath = Join-Path $Installation 'Nexus-OpenWrt-Desktop'
if ($Corrupt -eq 'yes') { $global:desktopTestVmPath = $Installation }
function global:Import-Module { param($Name) }
function global:Get-VM { param($Id); [pscustomobject]@{Name='Nexus-OpenWrt-Desktop'; Path=$global:desktopTestVmPath; State='Running'} }
function global:Get-VMSwitch { param($Id); [pscustomobject]@{SwitchType='Internal'; Name='test'} }
function global:Get-NetAdapter { param($Name); [pscustomobject]@{Name='test'; ifIndex=123} }
function global:Get-NetAdapterBinding { param($Name,$ComponentID); [pscustomobject]@{Enabled=$true} }
function global:Get-Command { param($Name,$ErrorAction) }
& $Script -InstallationDirectory $Installation -WhatIf
''', encoding='utf-8-sig')
        for recorded in (False, True):
            if recorded:
                state['vm_path'] = str(installation/state['vm_name'])
            (installation/'desktop-state.json').write_text(json.dumps(state), encoding='utf-8')
            for corrupt in (False, True):
                with self.subTest(recorded=recorded, corrupt=corrupt):
                    result = subprocess.run([shell, '-NoProfile', '-NonInteractive', '-File',
                        str(wrapper), str(HERE/'configure-ipv6.ps1'), str(installation),
                        'yes' if corrupt else 'no'], capture_output=True, timeout=30)
                    self.assertEqual(result.returncode == 0, not corrupt, result.stderr)

    def test_resume_tracks_hyperv_child_directory(self):
        shell = shutil.which('pwsh') or shutil.which('powershell')
        if not shell:
            self.skipTest('PowerShell is unavailable')
        installation = self.root/'installation'
        installation.mkdir()
        vm_name = 'Nexus-OpenWrt-Desktop'
        wrapper = self.root/'resume-test.ps1'
        wrapper.write_text('''param($Launcher, $Installation, $Corrupt)
$global:desktopTestInstallation = $Installation
$global:desktopTestVmPath = Join-Path $Installation 'Nexus-OpenWrt-Desktop'
if ($Corrupt -eq 'yes') { $global:desktopTestVmPath = Join-Path $Installation 'other-vm' }
function global:Import-Module { param($Name) }
function global:Get-VM {
    param($Id)
    [pscustomobject]@{Name='Nexus-OpenWrt-Desktop'; Path=$global:desktopTestVmPath; State='Off'; Status='Operating normally'}
}
function global:Get-VMHardDiskDrive {
    param($VM)
    [pscustomobject]@{Path=(Join-Path $global:desktopTestInstallation 'nexus-openwrt.vhdx')}
}
& $Launcher -Action Status -InstallationDirectory $Installation
''', encoding='utf-8-sig')
        for recorded in (False, True):
            state = dict(schema_version=1, installation=str(installation),
                         vm_id='00000000-0000-0000-0000-000000000001',
                         vm_name=vm_name, configured=True)
            if recorded:
                state['vm_path'] = str(installation/vm_name)
            (installation/'desktop-state.json').write_text(json.dumps(state), encoding='utf-8')
            for corrupt in (False, True):
                with self.subTest(recorded=recorded, corrupt=corrupt):
                    result = subprocess.run([shell, '-NoProfile', '-NonInteractive', '-File',
                        str(wrapper), str(HERE/'start-nexus-openwrt.ps1'), str(installation),
                        'yes' if corrupt else 'no'], capture_output=True, timeout=30)
                    self.assertEqual(result.returncode == 0, not corrupt, result.stderr)


if __name__ == '__main__':
    unittest.main()
