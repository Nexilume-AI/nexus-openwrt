from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _makefile(package: str) -> str:
    return (ROOT / "feed" / package / "Makefile").read_text(encoding="utf-8")


def test_core_package_releases_force_upgrade_of_same_version_development_images():
    assert "PKG_RELEASE:=25" in _makefile("agentd")
    assert "PKG_RELEASE:=26" in _makefile("agent-gw")
    assert "PKG_RELEASE:=7" in _makefile("agent-adapter")


def test_postinst_recovers_empty_conffiles_and_preserves_replaced_init_scripts():
    cases = {
        "agentd": ("/etc/config/$$name.apk-new", "/etc/init.d/agentd.pre-nexus-upgrade"),
        "agent-gw": ("/etc/config/agent_gateway.apk-new", "/etc/init.d/$$name.pre-nexus-upgrade"),
        "agent-adapter": ("/etc/config/agent_adapter.apk-new", "/etc/init.d/agent-adapter.pre-nexus-upgrade"),
    }
    for package, required in cases.items():
        source = _makefile(package)
        assert f"define Package/{package}/postinst" in source
        assert '[ -n "$${IPKG_INSTROOT}" ] && exit 0' in source
        assert required[0] in source
        assert required[1] in source
        assert "/etc/init.d/" in source and " enable" in source and " restart" in source
        assert "`t" not in source


def test_product_builder_and_mixed_harness_use_the_same_releases():
    builder = (ROOT / "scripts" / "build-product-sdk.sh").read_text(encoding="utf-8")
    harness = (ROOT / "scripts" / "hyperv-mixed-nat-acceptance.ps1").read_text(encoding="utf-8")
    for artifact in (
        "agentd-3.1.0-r19.apk",
        "agent-gw-0.22.0-r24.apk",
        "agent-adapter-0.6.0-r7.apk",
        "agent-netd-0.2.0-r2.apk",
        "nexus-cloud-connector-1.1.0-r34.apk",
        "luci-app-agent-router-3.1.0-r12.apk",
    ):
        assert artifact in builder
        assert artifact in harness
    assert "find \"$artifact_dir\" -maxdepth 1 -type f" in builder
    assert "-name 'nexus-cloud-connector-*.apk'" in builder
    assert '(cd "$artifact_dir" && sha256sum ./*.apk >SHA256SUMS)' in builder

def test_product_builder_uses_feed_directory_target_and_curl_kconfig_compatibility():
    builder = (ROOT / "scripts" / "build-product-sdk.sh").read_text(encoding="utf-8")
    assert "package/nexus-agent-services/compile" in builder
    assert "package/nexus-agent-roles/compile" not in builder
    assert "curl_config=\"$sdk_dir/feeds/packages/net/curl/Config.in\"" in builder
    assert "sed -i '1d;$d' \"$curl_config\"" in builder

def test_mixed_harness_rejects_corrupt_or_stale_product_packages():
    harness = (ROOT / "scripts" / "hyperv-mixed-nat-acceptance.ps1").read_text(encoding="utf-8")
    assert "${artifactHost}:${ArtifactPort}" in harness
    assert "sha256sum -c -" in harness
    assert "DEPENDENCY_APK_INSTALL_OK" in harness
    assert "PRODUCT_APK_INSTALL_OK" in harness
    assert "apk list --installed --manifest" in harness
    assert "uci set network.loopback=interface" in harness
    assert "ip addr replace 127.0.0.1/8 dev lo" in harness
    for version in (
        "agentd 3.1.0-r19",
        "agent-gw 0.22.0-r24",
        "agent-adapter 0.6.0-r7",
        "agent-netd 0.2.0-r2",
        "nexus-cloud-connector 1.1.0-r34",
        "luci-app-agent-router 3.1.0-r12",
    ):
        assert version in harness

def test_uci_reload_triggers_recreate_startup_configured_services():
    init_scripts = (
        "feed/agentd/files/agentd.init",
        "feed/agent-gw/files/agent-gw.init",
        "feed/agent-gw/files/agent-edge.init",
        "feed/agent-gw/files/agent-jwks.init",
        "feed/agent-netd/files/agent-netd.init",
        "feed/agent-adapter/files/agent-adapter.init",
    )
    for relative_path in init_scripts:
        source = (ROOT / relative_path).read_text(encoding="utf-8")
        assert "service_triggers()" in source
        assert "procd_add_reload_trigger" in source
        assert "reload_service()" in source
        assert "\tstop\n\tstart\n" in source
