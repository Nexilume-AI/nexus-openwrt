"""Run the real connector sync in a simulated long catalog without network."""
import pathlib, subprocess, unittest, os
SOURCE=pathlib.Path(os.environ.get('CONNECTOR_SOURCE', str(pathlib.Path(__file__).resolve().parents[1]/'feed/nexus-cloud-connector/files/nexus-cloud-connectord')))
class PresenceTests(unittest.TestCase):
 def test_long_catalog_and_failed_renewal(self):
  source=SOURCE.read_text();sync=source[source.index('sync_once() {'):source.index('withdraw_all() {')]
  harness=r'''
set -eu
RUNTIME_DIR=$(mktemp -d)
trap 'rm -rf "$RUNTIME_DIR"' EXIT
REGISTRATION_STATE=$RUNTIME_DIR/old
TAB="$(printf '\t')"
cloud_transport=relay
recovery_interval=30
sync_interval=30
FAIL_RENEW=0
UNHEALTHY=0
count=0
echo 1000 > "$RUNTIME_DIR/clock"
date() { cat "$RUNTIME_DIR/clock"; }
set_recovery_status() { :; }
clear_recovery_status() { :; }
write_status() { :; }
write_agent_registration_status() { :; }
next_generation() { echo 1; }
direct_ingress_ready() { return 1; }
extract_snapshot() {
 printf '1\tr\ta\t900\t1\n' > "$1/relay.tsv"
 : > "$1/local-origins"
 for n in $(seq 1 22); do
  mkdir "$1/agent-$n"
  echo "agent-$n" > "$1/agent-$n/origin"
  echo 1 > "$1/agent-$n/healthy"
  if [ "$UNHEALTHY" -eq 1 ] && [ "$n" -eq 1 ]; then echo 0 > "$1/agent-$n/healthy"; fi
  echo route > "$1/agent-$n/route-id"
  echo cap > "$1/agent-$n/capabilities"
  echo digest > "$1/agent-$n/manifest-digest"
  echo "agent-$n" >> "$1/local-origins"
 done
}
select_public_route() { printf 'route\taddress'; }
renew_presence() {
 echo "$1" >> "$RUNTIME_DIR/states"
 count=$((count+1))
 [ "$FAIL_RENEW" -eq 0 ] || [ "$count" -lt 2 ] || return 1
 date >> "$RUNTIME_DIR/renewals"
}
register_agent() {
 echo "$(( $(date) + 70 ))" > "$RUNTIME_DIR/clock"
 echo '{}' > "$6"
 echo id
}
json_data_value() { echo ready; }
b64_encode() { echo encoded; }
withdraw_registration() { echo unexpected-withdrawal; exit 90; }
old_id_for_origin() { :; }
'''
  assertions=r'''
sync_once
[ "$(wc -l < "$REGISTRATION_STATE")" -eq 22 ]
[ "$(wc -l < "$RUNTIME_DIR/renewals")" -eq 22 ]
awk 'NR>1 && $1-prev>=300 {exit 1} {prev=$1}' "$RUNTIME_DIR/renewals"
[ "$(sort -u "$RUNTIME_DIR/states")" = degraded ]
[ "$SYNC_PRESENCE_STATE" = online ]
: > "$RUNTIME_DIR/states"
sync_once
[ "$(sort -u "$RUNTIME_DIR/states")" = online ]
: > "$RUNTIME_DIR/states"
UNHEALTHY=1
sync_once
[ "$SYNC_PRESENCE_STATE" = degraded ]
[ "$(head -1 "$RUNTIME_DIR/states")" = online ]
[ "$(tail -1 "$RUNTIME_DIR/states")" = degraded ]
: > "$RUNTIME_DIR/states"
UNHEALTHY=0
sync_once
[ "$(sort -u "$RUNTIME_DIR/states")" = degraded ]
[ "$SYNC_PRESENCE_STATE" = online ]
cp "$REGISTRATION_STATE" "$RUNTIME_DIR/saved"
FAIL_RENEW=1
count=0
if sync_once; then exit 92; fi
cmp "$REGISTRATION_STATE" "$RUNTIME_DIR/saved"
[ "$SYNC_PRESENCE_STATE" = degraded ]
'''
  p=subprocess.run(['sh'],input=harness+sync+assertions,text=True,capture_output=True)
  self.assertEqual(p.returncode,0,p.stderr)
if __name__=='__main__':unittest.main()
