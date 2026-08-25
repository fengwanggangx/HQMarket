param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$output = Join-Path $root "contract"
$staging = Join-Path $output "hqmarket-contract-$Version"
New-Item -ItemType Directory -Path $staging -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root "quote\v1\market.proto") -Destination $staging
Copy-Item -LiteralPath (Join-Path $root "request\request.proto") -Destination $staging
Set-Content -LiteralPath (Join-Path $staging "VERSION") -Value $Version -NoNewline
Set-Content -LiteralPath (Join-Path $staging "CHANGELOG.md") -Value "# HQMarket Contract $Version`n`nInitial authentication, subscription, quote, depth, trade, bar, status and heartbeat contract."
$marketHash = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $staging "market.proto")
$requestHash = Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $staging "request.proto")
$hashes = "$($marketHash.Hash.ToLower())  market.proto`n$($requestHash.Hash.ToLower())  request.proto"
Set-Content -LiteralPath (Join-Path $staging "SHA256SUMS") -Value $hashes -NoNewline
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath (Join-Path $output "hqmarket-contract-$Version.zip") -Force
