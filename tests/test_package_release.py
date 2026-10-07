"""Release inventory and publication boundaries; no signing keys required."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("package_release", ROOT / "scripts/package-release.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class PackageReleaseTests(unittest.TestCase):
    def test_inventory_is_explicit_and_versioned(self):
        packages = release.inventory(ROOT)
        self.assertEqual(len(packages), 13)
        for name, item in packages.items():
            self.assertEqual(item["file"], f"{name}-{item['version']}.apk")
            self.assertNotIn("/", item["file"])
        self.assertNotIn("agent-cardd", packages)

    def test_reject_ambiguous_or_executable_recipe_values(self):
        for recipe in ("", "PKG_VERSION:=1\nPKG_VERSION:=2", "PKG_VERSION:=$(secret)"):
            with self.assertRaises(ValueError):
                release.recipe_field(recipe, "PKG_VERSION")

    def test_profiles_keep_node_optional(self):
        recipe = (ROOT / "feed/nexus-agent-profiles/Makefile").read_text(encoding="utf-8")
        base = recipe.split("define Package/nexus-agent-router\n")[1].split("endef")[0]
        self.assertNotIn("nexus-node-runtime", base)
        self.assertIn("+nexus-node-runtime", recipe)

    def test_install_never_disables_signature_verification(self):
        guide = (ROOT / "docs/package-install.md").read_text(encoding="utf-8")
        self.assertIn("apk verify /root/nexus-feed/packages.adb", guide)
        self.assertNotIn("apk --allow-untrusted", guide)

    def test_seed_installs_bounded_tls_probe_dependency(self):
        recipe = (ROOT / "feed/nexus-agent-services/Makefile").read_text(encoding="utf-8")
        directory = recipe.split("define Package/nexus-agent-directoryd\n")[1].split("endef")[0]
        self.assertIn("+coreutils-timeout", directory)
        preflight = (ROOT / "feed/nexus-agent-services/files/mesh-link.sh").read_text(encoding="utf-8")
        self.assertIn("command -v timeout", preflight)


if __name__ == "__main__":
    unittest.main()
