param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$output = Join-Path $root "contract"
$staging = Join-Path $output "hqmarket-contract-$Version"
New-Item -ItemType Directory -Path $staging -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root "market\v1\market.proto") -Destination $staging
Set-Content -LiteralPath (Join-Path $staging "VERSION") -Value $Version -NoNewline
Set-Content -LiteralPath (Join-Path $staging "CHANGELOG.md") -Value "# HQMarket Contract $Version`n`nInitial authentication, subscription, quote, depth, trade, bar, status and heartbeat contract."
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $staging "market.proto")
Set-Content -LiteralPath (Join-Path $staging "SHA256SUMS") -Value "$($hash.Hash.ToLower())  market.proto"
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath (Join-Path $output "hqmarket-contract-$Version.zip") -Force
