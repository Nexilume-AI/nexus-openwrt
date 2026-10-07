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
    assert "PKG_RELEASE:=36" in makefile
    assert "^PKG_RELEASE:=36$" in build_script
    assert "luci-app-agent-router-3.1.0-r36.apk" in build_script
    packager = (ROOT / 'scripts/build-agent-router-luci-apk.sh').read_text(encoding='utf-8')
    assert 's/#PKG_VERSION/$package_version/g' in packager
    assert 'Build/Prepare/luci-app-agent-router' in makefile
    assert 's/#PKG_VERSION/$(PKG_VERSION)-r$(PKG_RELEASE)/g' in makefile

def test_both_legacy_views_use_shared_mesh_connection_without_uci_writes():
    for name in ('settings.js', 'setup.js'):
        settings = (VIEWS / name).read_text(encoding='utf-8')
        assert 'meshSetup.render({ clientOnly: true })' in settings
        assert 'open_mesh_directory_connect_ipv4s' not in settings
        assert 'open_mesh_directory_endpoints' not in settings
        assert 'o.write = o.remove = function() {}' in settings
