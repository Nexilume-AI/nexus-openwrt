"""Execute the real role init scripts with isolated procd/UCI stubs.

On Windows use Git Bash; no router, host service or real /var is modified.
The device acceptance also verifies real service UID ownership and respawn.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

FILES = Path(__file__).resolve().parents[1] / "feed/nexus-agent-services/files"
BASH = (shutil.which("bash") if os.name != "nt" else
        "C:/Program Files/Git/bin/bash.exe")


class RoleRecoveryTests(unittest.TestCase):
    def run_init(self, role, root, mode="all", helper_override=None):
        helper_path = FILES / "role-runtime.sh"
        helper = helper_path.read_text() if helper_path.exists() else ""
        if helper_override is not None:
            helper = helper_override
        init = (FILES / f"nexus-{role}d.init").read_text()
        init = init.replace('. /usr/lib/nexus-agent-roles/role-runtime.sh', helper)
        init = init.replace("/var/lib", root.as_posix() + "/lib")
        config = root / "config.json"
        config.write_text("{}")
        script = f"""
config_load() {{ :; }}
config_get_bool() {{ eval "$1=0"; }}
config_get() {{
  case "$3" in
    mode) eval "$1={mode}" ;;
    config_file) eval "$1='{config.as_posix()}'" ;;
  esac
}}
procd_open_instance() {{ echo OPEN; }}
procd_close_instance() {{ :; }}
procd_set_param() {{ printf 'PARAM %s\\n' "$*"; }}
chown() {{ printf 'CHOWN %s\\n' "$*"; }}
chmod() {{ printf 'CHMOD %s\\n' "$*"; }}
mkdir() {{
  # Windows ACLs do not implement POSIX mode bits. Test the requested modes;
  # real ownership/mode checks run on the OpenWrt device.
  if [ "$1" = -m ]; then shift 2; fi
  command mkdir "$@"
}}
{init}
start_service
"""
        return subprocess.run([BASH, "--noprofile", "--norc", "-s"],
                              input=script.encode(), capture_output=True, timeout=10)

    def test_cold_boot_creates_private_directories_before_procd(self):
        for role, user in (("relay", "nexus-relay"), ("directory", "nexus-directory")):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                result = self.run_init(role, root)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertTrue((root / "lib" / f"nexus-{role}d").is_dir(),
                                "cold boot must recreate the volatile runtime directory")
                output = result.stdout.decode()
                self.assertLess(output.index("CHOWN"), output.index("OPEN"))
                self.assertIn(f"PARAM user {user}", output)
                self.assertIn(f"PARAM group {user}", output)
                self.assertIn("PARAM respawn 3600 30 0", output)
                self.assertIn("CHMOD 0700", output)
                # Repeated start repairs ownership without deleting diagnostic history.
                capture = root / "lib" / f"nexus-{role}d" / "evidence.txt"
                capture.write_text("preserve")
                again = self.run_init(role, root)
                self.assertEqual(again.returncode, 0)
                self.assertEqual(capture.read_text(), "preserve")

    def test_disabled_roles_do_not_create_or_start_anything(self):
        for role, mode in (("relay", "node"), ("directory", "node"),
                           ("relay", "node-directory"), ("directory", "node-relay")):
            with self.subTest(role=role, mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                result = self.run_init(role, root, mode=mode)
                self.assertEqual(result.returncode, 0)
                self.assertFalse((root / "lib").exists())
                self.assertNotIn(b"OPEN", result.stdout)

    def test_non_directory_preconditions_fail_closed(self):
        for role in ("relay", "directory"):
            for parent_blocked in (True, False):
                with self.subTest(role=role, parent=parent_blocked), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    blocked = root / "lib"
                    if not parent_blocked:
                        blocked.mkdir()
                        blocked /= f"nexus-{role}d"
                    blocked.write_text("do not modify")
                    result = self.run_init(role, root)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(blocked.read_text(), "do not modify")
                    self.assertNotIn(b"OPEN", result.stdout)
                    self.assertNotIn(b"CHOWN", result.stdout)

    def test_missing_config_still_has_slow_recovery(self):
        # A repaired config must be picked up without manually restarting an
        # exhausted instance. Node validates it under the service identity.
        for role in ("relay", "directory"):
            source = (FILES / f"nexus-{role}d.init").read_text()
            self.assertNotIn('[ -r "$config_file" ]', source)
            self.assertIn("procd_set_param respawn 3600 30 0", source)


class SeedReadinessTests(unittest.TestCase):
    def run_readiness(self, mode):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).as_posix()
            helper = (FILES / "mesh-link.sh").read_text()
            script = f"""
set -eu
clock='{root}/clock'
calls='{root}/calls'
printf 0 > "$clock"
printf 0 > "$calls"
date() {{ n=$(cat "$clock"); n=$((n+1)); printf '%s' "$n" > "$clock"; printf '%s' "$n"; }}
sleep() {{ :; }}
timeout() {{ test "$1" = 5 || return 99; shift; "$@"; }}
node() {{
  printf '%s\\n' "$*" >> '{root}/arguments'
  n=$(cat "$calls"); n=$((n+1)); printf '%s' "$n" > "$calls"
  case '{mode}' in
    delayed) [ "$n" -gt 3 ] ;;
    directory_down) case "$*" in *directory-seed*) return 1;; *) return 0;; esac ;;
    always_down) return 1 ;;
    ready) return 0 ;;
  esac
}}
{helper}
if mesh_seed_wait_ready /isolated/ca.pem 17444 18443; then result=0; else result=$?; fi
printf 'RESULT=%s CALLS=%s\\n' "$result" "$(cat "$calls")"
cat '{root}/arguments'
"""
            return subprocess.run([BASH, "--noprofile", "--norc", "-s"],
                                  input=script.encode(), capture_output=True, timeout=10)

    def test_waits_for_both_tls_listeners(self):
        for mode in ("ready", "delayed"):
            with self.subTest(mode=mode):
                result = self.run_readiness(mode)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertIn(b"RESULT=0", result.stdout)
                self.assertIn(b"17444 relay-seed.mesh.local /isolated/ca.pem", result.stdout)
                self.assertIn(b"18443 directory-seed.mesh.local /isolated/ca.pem", result.stdout)
                self.assertIn(b"rejectUnauthorized: true", result.stdout)
                self.assertIn(b"socket.authorized", result.stdout)
                self.assertIn(b'socket.alpnProtocol === "h2"', result.stdout)

    def test_unavailable_or_invalid_tls_is_bounded_and_fails_closed(self):
        for mode in ("always_down", "directory_down"):
            with self.subTest(mode=mode):
                result = self.run_readiness(mode)
                self.assertEqual(result.returncode, 0, result.stderr.decode())
                self.assertIn(b"RESULT=1", result.stdout)
                calls = int(result.stdout.decode().split("CALLS=")[1].splitlines()[0])
                self.assertLessEqual(calls, 40)


if __name__ == "__main__":
    unittest.main()
