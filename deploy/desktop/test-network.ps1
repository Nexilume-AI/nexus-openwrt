$ErrorActionPreference = 'Stop'
$tokens = $null
$errors = $null
$source = Join-Path $PSScriptRoot 'start-nexus-openwrt.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Launcher syntax errors' }
$definition = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Test-SubnetOverlap' }, $true)
. ([scriptblock]::Create($definition.Extent.Text))
$cases = @(
    @('192.168.246.99/32', '192.168.246.1', 24, $true),
    @('192.168.0.0/16', '192.168.246.1', 24, $true),
    @('192.168.245.0/24', '192.168.246.1', 24, $false),
    @('fd6e:6578:7573:246::99/128', 'fd6e:6578:7573:246::1', 64, $true),
    @('fd6e:6578:7573::/48', 'fd6e:6578:7573:246::1', 64, $true),
    @('fd6e:6578:7573:247::/64', 'fd6e:6578:7573:246::1', 64, $false)
)
foreach ($case in $cases) {
    if ((Test-SubnetOverlap $case[0] $case[1] $case[2]) -ne $case[3]) { throw "Wrong overlap decision: $($case[0])" }
}
[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'configure-ipv6.ps1'), [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count) { $errors; throw 'IPv6 script syntax errors' }
Write-Output 'Six IPv4/IPv6 collision checks and script parsing passed.'
