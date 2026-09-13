[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet('Start', 'Stop', 'Status', 'Check')][string]$Action = 'Start',
    [string]$BundleDirectory = $PSScriptRoot,
    [string]$InstallationDirectory = (Join-Path $env:LOCALAPPDATA 'Nexus\OpenWrtDesktop'),
    [ValidateRange(512, 16384)][int]$MemoryMiB = 1024,
    [ValidateRange(1, 16)][int]$Processors = 2
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-NoLink([string]$Path) {
    $candidate = [IO.Path]::GetFullPath($Path)
    while ($candidate) {
        if (Test-Path -LiteralPath $candidate) {
            if ((Get-Item -LiteralPath $candidate -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Linked paths are not supported: $candidate"
            }
        }
        $parent = Split-Path -Parent $candidate
        if ($parent -eq $candidate) { break }
        $candidate = $parent
    }
}

function Test-SubnetOverlap([string]$Prefix, [string]$Address, [int]$PrefixLength) {
    $parts = $Prefix.Split('/')
    $network = [Net.IPAddress]::Parse($parts[0]).GetAddressBytes()
    $target = [Net.IPAddress]::Parse($Address).GetAddressBytes()
    if ($network.Length -ne $target.Length) { return $false }
    $bits = [Math]::Min([int]$parts[1], $PrefixLength)
    for ($i = 0; $i -lt $target.Length; $i++) {
        $used = [Math]::Min(8, [Math]::Max(0, $bits - 8 * $i))
        $mask = if ($used -eq 0) { 0 } else { (255 -shl (8 - $used)) -band 255 }
        if (($network[$i] -band $mask) -ne ($target[$i] -band $mask)) { return $false }
    }
    return $true
}

$installation = [IO.Path]::GetFullPath($InstallationDirectory)
Assert-NoLink $installation
$statePath = Join-Path $installation 'desktop-state.json'
$state = $null
if (Test-Path -LiteralPath $statePath) {
    Assert-NoLink $statePath
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($state.schema_version -ne 1 -or $state.installation -ne $installation) { throw 'Installation state does not match this directory.' }
}

# Check validates release bytes without creating a VM or requiring administrator rights.
if ($Action -eq 'Check' -or ($Action -eq 'Start' -and $null -eq $state)) {
    $bundle = [IO.Path]::GetFullPath($BundleDirectory)
    Assert-NoLink $bundle
    $manifestPath = Join-Path $bundle 'desktop-image.json'
    Assert-NoLink $manifestPath
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema_version -ne 1 -or $manifest.profile -ne 'nexus-openwrt-hyperv-v1' -or
        $manifest.architecture -ne 'x86_64' -or $manifest.generation -ne 2 -or
        $manifest.lan_address -ne '192.168.246.1' -or $manifest.host_address -ne '192.168.246.2' -or
        $manifest.lan_ipv6 -ne 'fd6e:6578:7573:246::1' -or $manifest.host_ipv6 -ne 'fd6e:6578:7573:246::2') {
        throw 'Unsupported desktop image profile.'
    }
    if ($manifest.file -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*\.vhdx$' -or $manifest.sha256 -notmatch '^[a-fA-F0-9]{64}$') {
        throw 'Invalid image filename or SHA-256.'
    }
    $imagePath = Join-Path $bundle $manifest.file
    Assert-NoLink $imagePath
    if ((Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash -ne $manifest.sha256) { throw 'Image checksum mismatch.' }
    if ($Action -eq 'Check') {
        Write-Output 'Release image checksum and desktop profile OK. This does not verify VM boot or publisher identity.'
        return
    }
}

Import-Module Hyper-V -ErrorAction Stop
if ($null -ne $state) {
    $vm = Get-VM -Id ([guid]$state.vm_id) -ErrorAction Stop
    # Hyper-V creates a per-VM child directory beneath New-VM -Path.
    $expectedVmPath = if ($state.PSObject.Properties.Name -contains 'vm_path') {
        [IO.Path]::GetFullPath($state.vm_path)
    } else {
        # Compatibility with the first launcher, which recorded only its parent.
        [IO.Path]::GetFullPath((Join-Path $installation $state.vm_name))
    }
    if ($expectedVmPath -ne [IO.Path]::GetFullPath((Join-Path $installation $state.vm_name)) -or
        $vm.Name -ne $state.vm_name -or [IO.Path]::GetFullPath($vm.Path) -ne $expectedVmPath) {
        throw 'Tracked VM identity/path changed; refusing to operate.'
    }
    $disk = @(Get-VMHardDiskDrive -VM $vm)
    if ($disk.Count -ne 1 -or $disk[0].Path -ne (Join-Path $installation 'nexus-openwrt.vhdx')) { throw 'Tracked VM disk changed.' }
    if ($Action -eq 'Status') {
        $vm | Select-Object Name, State, Status
        Write-Output 'Management: http://192.168.246.1/ (VM running does not prove Agent readiness.)'
        Write-Output 'IPv6 management: http://[fd6e:6578:7573:246::1]/ (private ULA, not public IPv6).'
        return
    }
    if ($Action -eq 'Stop') {
        if ($vm.State -eq 'Running' -and $PSCmdlet.ShouldProcess($vm.Name, 'Request graceful guest shutdown')) {
            Stop-VM -VM $vm -Confirm:$false
        }
        return
    }
    if ($state.configured -ne $true) { throw 'Initial setup did not finish. Inspect this VM and its dedicated adapter before retrying; it will not be started automatically.' }
    if ($vm.State -eq 'Running') { Write-Output 'VM is already running: http://192.168.246.1/'; return }
    if ($PSCmdlet.ShouldProcess($vm.Name, 'Start existing desktop VM')) { Start-VM -VM $vm | Out-Null }
    return
}
if ($Action -ne 'Start') { throw 'No managed desktop installation in this directory.' }

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'First start requires an Administrator PowerShell with Hyper-V enabled.'
}
$vmName = 'Nexus-OpenWrt-Desktop'
$switchName = 'Nexus-OpenWrt-Desktop-LAN'
if (Get-VM -Name $vmName -ErrorAction SilentlyContinue) { throw "VM already exists without matching state: $vmName" }
if (Get-VMSwitch -Name $switchName -ErrorAction SilentlyContinue) { throw "Switch already exists: $switchName" }
if (Test-Path -LiteralPath $installation) { throw 'Choose a new installation directory; existing directories are never adopted.' }
# Refuse the dedicated /24 if it overlaps a configured address or non-default route.
foreach ($address in @(Get-NetIPAddress -AddressFamily IPv4)) {
    if ($address.IPAddress -like '192.168.246.*') { throw 'Desktop management subnet is already in use.' }
}
foreach ($route in @(Get-NetRoute -AddressFamily IPv4)) {
    if ($route.DestinationPrefix -ne '0.0.0.0/0' -and (Test-SubnetOverlap $route.DestinationPrefix '192.168.246.1' 24)) {
        throw "Desktop subnet overlaps route $($route.DestinationPrefix)."
    }
}
foreach ($route in @(Get-NetRoute -AddressFamily IPv6)) {
    if ($route.DestinationPrefix -ne '::/0' -and (Test-SubnetOverlap $route.DestinationPrefix 'fd6e:6578:7573:246::1' 64)) {
        throw "Desktop IPv6 subnet overlaps route $($route.DestinationPrefix)."
    }
}
if (-not $PSCmdlet.ShouldProcess($installation, 'Create isolated Hyper-V desktop VM and management adapter')) { return }
New-Item -ItemType Directory -Path $installation | Out-Null
$diskPath = Join-Path $installation 'nexus-openwrt.vhdx'
Copy-Item -LiteralPath $imagePath -Destination $diskPath
$switch = New-VMSwitch -Name $switchName -SwitchType Internal
$vm = New-VM -Name $vmName -Generation 2 -MemoryStartupBytes ($MemoryMiB * 1MB) -Path $installation -VHDPath $diskPath -SwitchName $switchName
# Persist identity before further configuration. On failure retain objects for diagnosis.
$state = [ordered]@{schema_version=1; installation=$installation; vm_id=[string]$vm.Id; vm_name=$vmName; vm_path=[IO.Path]::GetFullPath($vm.Path); switch_id=[string]$switch.Id; image_sha256=$manifest.sha256; configured=$false}
$state |
    ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
$lanAdapter = Get-VMNetworkAdapter -VM $vm | Select-Object -First 1
Rename-VMNetworkAdapter -VMNetworkAdapter $lanAdapter -NewName 'LAN'
Set-VMProcessor -VM $vm -Count $Processors
Set-VMMemory -VM $vm -DynamicMemoryEnabled $false
Set-VMFirmware -VM $vm -EnableSecureBoot Off
Set-VM -VM $vm -AutomaticStartAction Nothing -AutomaticStopAction ShutDown -CheckpointType Disabled
$adapter = Get-NetAdapter -Name "vEthernet ($switchName)" -ErrorAction Stop
Set-NetIPInterface -InterfaceIndex $adapter.ifIndex -AddressFamily IPv4 -Dhcp Disabled -AutomaticMetric Disabled -InterfaceMetric 5000
New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress '192.168.246.2' -PrefixLength 24 | Out-Null
New-NetIPAddress -InterfaceIndex $adapter.ifIndex -IPAddress 'fd6e:6578:7573:246::2' -PrefixLength 64 | Out-Null
$state.configured = $true
$state | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
Start-VM -VM $vm | Out-Null
Write-Output 'VM started. Open http://192.168.246.1/ after guest boot; set a unique root password, then open Status > Agent Routing > User mode.'
Write-Output 'Private IPv6: guest fd6e:6578:7573:246::1, host fd6e:6578:7573:246::2. Public IPv6 requires a separately configured upstream.'
Write-Output 'No WAN or physical bridge was connected. Follow README.md before enabling Internet or remote Agents.'
