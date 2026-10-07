"""Execute the production shell parser with real OpenSSL.

Use native jshn on OpenWrt. On other hosts only the jshn variable adapter is
substituted; decoding, validation, hashing, certificate parsing and paths are
the production implementation. No router configuration is modified.
"""
import base64
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[1]
PARSER = ROOT / "feed/nexus-cloud-connector/files/pairing-link.sh"
BASH = "C:/Program Files/Git/bin/bash.exe" if os.name == "nt" else shutil.which("bash")
pytestmark = pytest.mark.skipif(not BASH or not Path(BASH).exists(), reason="A POSIX shell is required")


def payload():
    return dict(version=1, cloud="https://cloud.example:28443", code="pair_test_secret",
                expires_at=int(time.time()) + 600, trust={"mode": "system"}, edge_trust={"mode": "system"})


def certificate():
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Pairing parser test")])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(datetime.now(timezone.utc) - timedelta(days=1))
            .not_valid_after(datetime.now(timezone.utc) + timedelta(days=1))
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True).sign(key, hashes.SHA256()))
    return cert.public_bytes(serialization.Encoding.PEM).decode()


def trust(pem):
    return dict(mode="pinned-pem", ca_pem=pem, sha256=hashlib.sha256(pem.encode()).hexdigest())


def json_adapter(data, raw):
    values, types = [], []
    for prefix, obj in (("", data), ("trust", data.get("trust", {})), ("edge_trust", data.get("edge_trust", {}))):
        if not isinstance(obj, dict):
            continue
        for key, value in obj.items():
            kind = "object" if isinstance(value, dict) else "int" if type(value) is int else "string"
            case = shlex.quote(prefix + ":" + key)
            values.append(f"{case}) printf -v \"$1\" '%s' {shlex.quote(str(value))};;")
            types.append(f"{case}) printf -v \"$1\" '%s' {kind};;")
    return f"""
json_load() {{ _path=''; [ "$1" = {shlex.quote(raw)} ]; }}
json_select() {{ if [ "$1" = .. ]; then _path=''; else _path="$1"; fi; }}
json_get_var() {{ case "$_path:$2" in {' '.join(values)} *) printf -v "$1" '%s' '';; esac; }}
json_get_type() {{ case "$_path:$2" in {' '.join(types)} *) printf -v "$1" '%s' '';; esac; }}
"""


def parse(data, *, suffix="", input_link=None):
    raw = json.dumps(data, separators=(",", ":"))
    link = input_link or "nexus-router://pair/v1/" + base64.urlsafe_b64encode(raw.encode()).decode().rstrip("=") + suffix
    with tempfile.TemporaryDirectory(prefix="nexus-pairing-test-") as directory:
        # Fixtures never enter command arguments or logs. stdin only.
        script = "set -u\n" + json_adapter(data, raw) + PARSER.read_text(encoding="utf-8")
        script += f"\nif parse_pairing_link {shlex.quote(link)} {shlex.quote(Path(directory).as_posix())}; then printf 'PASS\\n'; else printf 'FAIL:%s\\n' \"$PAIRING_ERROR\"; fi\n"
        result = subprocess.run([str(BASH), "--noprofile", "--norc", "-s"], input=script, text=True, capture_output=True, timeout=15)
    assert result.returncode == 0, result.stderr
    assert "pair_test_secret" not in result.stdout + result.stderr
    return result.stdout.strip()


def test_system_trust_and_explicit_port():
    assert parse(payload()) == "PASS"


def test_separate_public_and_edge_certificate_bundles():
    data = payload()
    data["trust"] = trust(certificate())
    data["edge_trust"] = trust(certificate() + certificate())
    assert parse(data) == "PASS"


@pytest.mark.parametrize("origin", ["http://cloud.example", "https://user@cloud.example", "https://cloud.example/api", "https://cloud.example?x=1", "https://cloud.example#x", "https://cloud.example:0", "https://cloud.example:65536", "https://cloud.example\n", 'https://cloud.example"', "https://$(touch /tmp/injected)"])
def test_unsafe_origin_rejected(origin):
    data = payload()
    data["cloud"] = origin
    assert parse(data).startswith("FAIL:")


@pytest.mark.parametrize("origin", ["https://cloud.example", "https://192.168.250.164:28443", "https://[2001:db8::1]:28443"])
def test_valid_origins(origin):
    data = payload()
    data["cloud"] = origin
    assert parse(data) == "PASS"


def test_expired_and_unknown_version_and_invalid_token():
    data = payload()
    data["expires_at"] = int(time.time()) - 1
    assert parse(data) == "FAIL:PAIRING_LINK_EXPIRED"
    for field, value in (("version", 2), ("code", "$(id)"), ("version", "1"), ("expires_at", "tomorrow")):
        data = payload()
        data[field] = value
        assert parse(data).startswith("FAIL:")


@pytest.mark.parametrize("suffix", ["=", "%0A", "/extra", "#fragment", " " , "x" * 50000], ids=["padding", "escape", "path", "fragment", "space", "oversize"])
def test_link_encoding_and_size(suffix):
    assert parse(payload(), suffix=suffix).startswith("FAIL:")


def test_ca_corruption_private_keys_and_unexpected_pem_rejected():
    ca = certificate()
    for pem in (ca + "-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----\n", ca + "unexpected\n", "x" * 12289, ca.replace("MI", "XX", 1)):
        data = payload()
        data["trust"] = trust(pem)
        assert parse(data).startswith("FAIL:")
    data = payload()
    data["trust"] = trust(ca)
    data["trust"]["sha256"] = "0" * 64
    assert parse(data).startswith("FAIL:")


def test_no_network_fetch_no_disabled_tls_and_packaged_helper():
    source = PARSER.read_text(encoding="utf-8")
    assert "curl" not in source and "wget" not in source and "eval " not in source
    # agent-gw runs as an unprivileged user and must read public CA material.
    # Staging remains private; only the validated certificate is made readable.
    assert 'chmod 0644 "$target.tmp" && mv -f' in source
    makefile = (ROOT / "feed/nexus-cloud-connector/Makefile").read_text()
    assert "./files/pairing-link.sh" in makefile
    rpc = (ROOT / "feed/luci-app-agent-router/root/usr/libexec/rpcd/nexus-agent-ui").read_text()
    assert "CLOUD_IDENTITY_CONFLICT" in rpc and "CONFIGURATION_CHANGED" in rpc
    for field in ("ca_file old_pair_ca", "public_ca_file old_pair_public_ca", "identity_mode old_pair_identity"):
        assert "restore_option nexus_cloud main " + field in rpc


@pytest.mark.parametrize("mode,existing,expected", [
    ("managed", True, "CSR"), ("managed", False, "CSR"),
    ("manual", True, "FINGERPRINT"), ("manual", False, "REJECTED"),
])
def test_managed_pairing_always_reissues_device_certificate(mode, existing, expected):
    source = (ROOT / "feed/nexus-cloud-connector/files/nexus-cloud-connectord").read_text()
    enrollment = source.split("enroll_node() {", 1)[1].split("\nnext_generation()", 1)[0]
    selection = enrollment[enrollment.index('\tif [ "$identity_mode" = managed ]'):enrollment.index('\n\trelease=')]
    script = f'''
identity_mode={shlex.quote(mode)}
REENROLLING=0
client_key=/dev/null
client_cert=/dev/null
prepare_managed_identity() {{ printf test-csr; }}
certificate_thumbprint() {{ {"printf test-fingerprint" if existing else "return 1"}; }}
certificate_key_matches() {{ return 0; }}
write_status() {{ :; }}
select_identity() {{
local csr_file='' fingerprint=''
{selection}
if [ -n "$csr_file" ]; then printf CSR; else printf FINGERPRINT; fi
}}
select_identity || printf REJECTED
'''
    result = subprocess.run([str(BASH), "--noprofile", "--norc", "-s"], input=script,
                            text=True, capture_output=True, timeout=15)
    assert result.returncode == 0, result.stderr
    assert result.stdout == expected


@pytest.mark.parametrize("scenario,expected", [
    ("success", "OK:pairing"), ("generation", "ERROR:CONFIGURATION_CHANGED"),
    ("restart", "ERROR:APPLY_FAILED"), ("commit", "ERROR:APPLY_FAILED"),
    ("other_cloud", "ERROR:CLOUD_IDENTITY_CONFLICT"),
])
def test_rpc_transaction_and_identity_guard(scenario, expected):
    """Run the real RPC transaction with isolated UCI/service adapters."""
    source = (ROOT / "feed/luci-app-agent-router/root/usr/libexec/rpcd/nexus-agent-ui").read_text()
    transaction = source.split("pair_cloud() {", 1)[1].split("\nrefresh_status()", 1)[0]
    transaction = "pair_cloud() {" + transaction
    transaction = transaction.replace("[ -r /usr/share/nexus-cloud/pairing-link.sh ]", "true")
    transaction = transaction.replace(". /usr/share/nexus-cloud/pairing-link.sh", ":")
    # Extract only the two unchanged snapshot functions, not feature controls.
    options = source[source.index("remember_option() {"):source.index("\n}", source.index("restore_option() {")) + 2]
    with tempfile.TemporaryDirectory(prefix="nexus-rpc-test-") as directory:
        token = Path(directory) / "token"
        if scenario == "other_cloud":
            token.write_text("test-existing-device-identity")
        script = f"""
declare -A config
config[nexus_cloud.main.base_url]='https://old.example:9443'
config[nexus_cloud.main.enrollment_url]='https://old.example:8443'
config[nexus_cloud.main.ca_file]='/etc/ssl/certs/original.pem'
config[nexus_cloud.main.enabled]=0
config[nexus_cloud.main.token_file]={shlex.quote(token.as_posix())}
scenario={shlex.quote(scenario)}
commits=0
installs=0
uci() {{
  [ "$1" != -q ] || shift
  case "$1" in
    get) [ "${{config[$2]+present}}" ] && printf '%s' "${{config[$2]}}" ;;
    set) config["${{2%%=*}}"]="${{2#*=}}" ;;
    delete) unset 'config[$2]' ;;
    commit) commits=$((commits+1)); [ "$scenario" != commit ] || [ "$commits" -gt 1 ] ;;
    *) return 1 ;;
  esac
}}
uci_value() {{ uci get "$1.$2.$3" || printf '%s' "$4"; }}
json_load() {{ return 0; }}
json_get_var() {{
  case "$2" in
    pairing_code) printf -v "$1" '%s' nexus-router://pair/v1/fixture;;
    expected_generation) printf -v "$1" '%s' 7;;
  esac
}}
parse_pairing_link() {{
  PAIRING_ORIGIN='https://new.example:28443'
  PAIRING_CODE=pair_fixture
  PAIRING_PUBLIC_CA=/etc/agent-gw/new-public.pem
  PAIRING_EDGE_CA=/etc/agent-gw/new-edge.pem
}}
install_pairing_trust() {{ installs=$((installs+1)); }}
acquire_ui_lock() {{ return 0; }}
release_ui_lock() {{ return 0; }}
config_generation() {{ if [ "$scenario" = generation ]; then printf 8; else printf 7; fi; }}
cloud_profile_value() {{ return 0; }}
reload_feature() {{ [ "$scenario" != restart ]; }}
wait_for_feature() {{ return 0; }}
json_reply_error() {{ printf 'ERROR:%s\\n' "$1"; }}
json_reply_ok() {{ printf 'OK:%s\\n' "$1"; }}
{options}
{transaction}
pair_cloud <<'REQUEST'
{{}}
REQUEST
if [ "$scenario" = success ]; then
  [ "${{config[nexus_cloud.main.base_url]}}" = https://new.example:28443 ] || exit 11
  [ "${{config[nexus_cloud.main.public_ca_file]}}" = /etc/agent-gw/new-public.pem ] || exit 12
  [ "${{config[nexus_cloud.main.ca_file]}}" = /etc/agent-gw/new-edge.pem ] || exit 13
else
  [ "${{config[nexus_cloud.main.base_url]}}" = https://old.example:9443 ] || exit 21
  [ "${{config[nexus_cloud.main.enrollment_url]}}" = https://old.example:8443 ] || exit 22
  [ "${{config[nexus_cloud.main.ca_file]}}" = /etc/ssl/certs/original.pem ] || exit 23
  [ -z "${{config[nexus_cloud.main.public_ca_file]+present}}" ] || exit 24
  [ -z "${{config[nexus_cloud.main.pairing_code]+present}}" ] || exit 25
  [ "${{config[nexus_cloud.main.enabled]}}" = 0 ] || exit 26
fi
case "$scenario" in generation|other_cloud) [ "$installs" = 0 ] || exit 30 ;; esac
"""
        result = subprocess.run([str(BASH), "--noprofile", "--norc", "-s"], input=script, text=True, capture_output=True, timeout=15)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == expected
