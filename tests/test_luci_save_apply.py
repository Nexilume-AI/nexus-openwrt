from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VIEWS = ROOT / "feed" / "luci-app-agent-router" / "htdocs" / "luci-static" / "resources" / "view" / "agent-router"


def test_all_config_views_save_and_apply_in_one_visible_action():
    for name in ("setup.js", "settings.js", "protocols.js", "roles.js", "cloud.js", "peers.js", "policies.js"):
        source = (VIEWS / name).read_text(encoding="utf-8")
        assert "'require ui';" in source, name
        assert "handleSave: function(ev)" in source, name
        assert "this.super('handleSave', [ev])" in source, name
        assert "ui.changes.apply(false)" in source, name
        assert "`t" not in source, name


def test_luci_release_and_device_build_artifact_match():
    makefile = (ROOT / "feed" / "luci-app-agent-router" / "Makefile").read_text(encoding="utf-8")
    build_script = (ROOT / "scripts" / "build-device-tls-sdk.sh").read_text(encoding="utf-8")
    assert "PKG_RELEASE:=21" in makefile
    assert "^PKG_RELEASE:=21$" in build_script
    assert "luci-app-agent-router-3.1.0-r21.apk" in build_script

def test_directory_ipv4_pins_allow_same_address_for_multiple_urls():
    settings = (VIEWS / "settings.js").read_text(encoding="utf-8")
    marker = "relay_directory_connect_ipv4s', _('Optional fixed Directory IPv4 addresses'))"
    start = settings.index(marker)
    assert "o.allowduplicates = true;" in settings[start:start + 300]
