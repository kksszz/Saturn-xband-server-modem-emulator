param([Parameter(Mandatory=$true)][string]$YmirSource)
$ErrorActionPreference = 'Stop'
$integrationSource = (Resolve-Path -LiteralPath $YmirSource).Path
$integrationBase = '9a237ea6642912ae0833f691809aa47dc27f908d'
$integrationPatch = Join-Path $PSScriptRoot 'ymir-9a237ea-serial-xband.patch'
$integrationHead = (& git -C $integrationSource rev-parse HEAD)
if ($LASTEXITCODE -ne 0 -or $integrationHead -ne $integrationBase) {
    throw "Use a separate YMIR checkout at $integrationBase. No files were changed."
}
$integrationChanges = & git -C $integrationSource status --porcelain --untracked-files=no --ignore-submodules=all
if ($LASTEXITCODE -ne 0 -or $integrationChanges) {
    throw 'Tracked changes exist. Use a clean, separate checkout; do not reset your existing work.'
}
& git -C $integrationSource apply --check $integrationPatch
if ($LASTEXITCODE -ne 0) { throw 'Patch check failed. No patch was applied.' }
& git -C $integrationSource apply $integrationPatch
if ($LASTEXITCODE -ne 0) { throw 'Patch application failed. Inspect git status before continuing.' }
& git -C $integrationSource apply --reverse --check $integrationPatch
if ($LASTEXITCODE -ne 0) { throw 'Applied patch verification failed.' }
Write-Output 'PASS Serial Port, battle cable, XBAND modem and virtual-card UI integration applied.'
Write-Output 'YMIR has not been built or started. Follow docs/YMIR-INTEGRATION-MANUAL.md.'
