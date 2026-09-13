[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet('Local', 'Upstream')][string]$Mode = 'Local',
    [string]$InstallationDirectory = (Join-Path $env:LOCALAPPDATA 'Nexus\OpenWrtDesktop'),
    [string]$RouterAddress = '192.168.246.1',
    [ValidatePattern('^[a-zA-Z0-9_.:-]{0,15}$')][string]$WanDevice = '',
    [string]$WanSwitchName = '',
    [string]$IdentityFile = '',
    [string]$KnownHostsFile = ''
)
$ErrorActionPreference = 'Stop'
$address = $null
if (-not [Net.IPAddress]::TryParse($RouterAddress, [ref]$address)) { throw 'RouterAddress must be a literal IP address.' }
Import-Module Hyper-V
$installation = [IO.Path]::GetFullPath($InstallationDirectory)
$state = Get-Content -LiteralPath (Join-Path $installation 'desktop-state.json') -Raw | ConvertFrom-Json
if ($state.installation -ne $installation -or $state.configured -ne $true) { throw 'Desktop installation is not configured.' }
$vm = Get-VM -Id ([guid]$state.vm_id)
$expectedVmPath = [IO.Path]::GetFullPath((Join-Path $installation $state.vm_name))
if (($state.vm_path -and [IO.Path]::GetFullPath($state.vm_path) -ne $expectedVmPath) -or
    $vm.Name -ne $state.vm_name -or [IO.Path]::GetFullPath($vm.Path) -ne $expectedVmPath -or $vm.State -ne 'Running') {
    throw 'The tracked desktop VM must be running with its original identity.'
}
$switch = Get-VMSwitch -Id ([guid]$state.switch_id)
if ($switch.SwitchType -ne 'Internal') { throw 'Management switch must remain Internal.' }
$adapter = Get-NetAdapter -Name "vEthernet ($($switch.Name))"
if (-not (Get-NetAdapterBinding -Name $adapter.Name -ComponentID ms_tcpip6).Enabled) { throw 'Enable IPv6 on the dedicated management adapter before continuing.' }
Get-Command ssh.exe -ErrorAction Stop | Out-Null
if (-not $PSCmdlet.ShouldProcess($vm.Name, "Configure $Mode IPv6 on this desktop VM and dedicated host adapter")) { return }

$wanted = 'fd6e:6578:7573:246::2'
$existing = @(Get-NetIPAddress -AddressFamily IPv6 -IPAddress $wanted -ErrorAction SilentlyContinue)
if ($existing.Count -and @($existing | Where-Object InterfaceIndex -ne $adapter.ifIndex).Count) { throw 'Desktop IPv6 address belongs to another adapter.' }
if (-not $existing.Count) { New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress $wanted -PrefixLength 64 | Out-Null }
if ($Mode -eq 'Upstream' -and $WanSwitchName) {
    $wanSwitch = Get-VMSwitch -Name $WanSwitchName
    if ($wanSwitch.Id -eq $switch.Id) { throw 'WAN must not use the management switch.' }
    $wan = @(Get-VMNetworkAdapter -VM $vm | Where-Object Name -eq 'WAN')
    if ($wan.Count -gt 1) { throw 'Multiple WAN adapters; inspect the VM first.' }
    if ($wan.Count -eq 1 -and $wan[0].SwitchId -ne $wanSwitch.Id) { throw 'Existing WAN belongs to another switch; it will not be moved.' }
    if (-not $wan.Count) { Add-VMNetworkAdapter -VM $vm -Name WAN -SwitchName $wanSwitch.Name }
}

# No password is stored or passed in arguments. OpenSSH prompts normally and verifies host keys.
$guestScript = @'
set -eu
mode=$1
requested=${2:-}
if [ "$mode" = Local ]; then
  uci set network.lan.ip6addr='fd6e:6578:7573:246::1/64'
  uci commit network
  /etc/init.d/network reload
  echo 'LOCAL_IPV6_CONFIGURED fd6e:6578:7573:246::1/64'
  exit 0
fi
wan=$requested
if [ -z "$wan" ]; then wan=$(uci -q get network.wan.device || true); fi
if [ -z "$wan" ]; then
  candidates=
  count=0
  for path in /sys/class/net/*; do
    name=${path##*/}
    [ "$name" != lo ] || continue
    [ ! -d "$path/bridge" ] || continue
    [ ! -e "$path/master" ] || continue
    [ "$(cat "$path/type")" = 1 ] || continue
    candidates=$name
    count=$((count + 1))
  done
  [ "$count" -eq 1 ] || { echo 'Cannot identify one unused WAN device; specify -WanDevice after inspecting LuCI.' >&2; exit 2; }
  wan=$candidates
fi
case "$wan" in ''|*[!a-zA-Z0-9_.:-]*) exit 2;; esac
[ -d "/sys/class/net/$wan" ] && [ ! -d "/sys/class/net/$wan/bridge" ] && [ ! -e "/sys/class/net/$wan/master" ] && [ "$wan" != lo ] || { echo 'Refusing to use LAN/bridge/unknown device as WAN.' >&2; exit 2; }
# Require an existing standard WAN firewall zone; never open management on WAN.
zone=
for item in $(uci show firewall | sed -n 's/^firewall\.\([^=]*\)=zone$/\1/p'); do
  [ "$(uci -q get "firewall.$item.name" || true)" != wan ] || zone=$item
done
[ -n "$zone" ] || { echo 'Standard WAN firewall zone missing; configure it before continuing.' >&2; exit 2; }
uci set network.lan.ip6addr='fd6e:6578:7573:246::1/64'
uci set network.wan=interface
uci set network.wan.device="$wan"
uci set network.wan.proto=dhcp
uci set network.wan6=interface
uci set network.wan6.device="$wan"
uci set network.wan6.proto=dhcpv6
uci set network.wan6.reqaddress=try
uci set network.wan6.reqprefix=auto
for name in wan wan6; do
  case " $(uci -q get "firewall.$zone.network" || true) " in *" $name "*) ;; *) uci add_list "firewall.$zone.network=$name";; esac
done
uci commit firewall
uci commit network
/etc/init.d/firewall reload
/etc/init.d/network reload
sleep 8
echo 'WAN6_STATUS (an empty ipv6-address/prefix is NOT public IPv6 success):'
ubus call network.interface.wan6 status
echo 'DHCPv6 configured. Public address/prefix and external reachability require upstream support.'
'@
$hostKeyPolicy = if ($KnownHostsFile) { 'StrictHostKeyChecking=yes' } else { 'StrictHostKeyChecking=ask' }
$sshOptions = @('-T', '-o', $hostKeyPolicy)
if ($IdentityFile) { $sshOptions += @('-i', (Resolve-Path -LiteralPath $IdentityFile).Path, '-o', 'BatchMode=yes') }
if ($KnownHostsFile) {
    $sshOptions += @('-o', ('UserKnownHostsFile=' + (Resolve-Path -LiteralPath $KnownHostsFile).Path))
}
$guestScript | & ssh.exe @sshOptions "root@$RouterAddress" "tr -d '\r' | sh -s -- $Mode $WanDevice"
if ($LASTEXITCODE -ne 0) { throw 'Guest IPv6 configuration failed. Inspect the reported cause before retrying.' }
Write-Output 'Private IPv6 management: http://[fd6e:6578:7573:246::1]/'
Write-Output 'Local ULA is not globally routable. Upstream mode reports DHCPv6 status and does not claim inbound Internet reachability.'
