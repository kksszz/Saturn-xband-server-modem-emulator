param([string]$Version='v0.2.0')
$ErrorActionPreference='Stop'
if($Version -notmatch '^v\d+\.\d+\.\d+([.-][a-zA-Z0-9.-]+)?$'){throw 'Invalid release version'}
$taskStage=Join-Path $PSScriptRoot ('artifacts/'+$Version+'/Saturn-xband-server-modem-emulator-windows-x64')
$taskZip=Join-Path (Split-Path $taskStage) ('Saturn-xband-server-modem-emulator-'+$Version+'-windows-x64.zip')
if((Test-Path -LiteralPath $taskStage) -or (Test-Path -LiteralPath $taskZip)){throw 'Preserve existing release; use a new version'}
$taskMapping=@{
    'build/Release/xband-server.exe'='bin/xband-server.exe'
    'start-server.ps1'='start-server.ps1'
    'README.md'='README.md'
    'RELEASE-NOTES.md'='RELEASE-NOTES.md'
    'THIRD-PARTY-NOTICES.md'='THIRD-PARTY-NOTICES.md'
    'config/jp-area-codes.json'='config/jp-area-codes.json'
    'docs/XBAND-COMMAND-SPEC-DRAFT.md'='docs/XBAND-COMMAND-SPEC-DRAFT.md'
    'docs/SERVER-MODEM-SPEC.md'='docs/SERVER-MODEM-SPEC.md'
    'docs/SERVER-MODEM-SPEC-v0.1.0-ja.pdf'='docs/SERVER-MODEM-SPEC-v0.1.0-ja.pdf'
    'docs/BATTLE-CABLE-SERIAL-SPEC.md'='docs/BATTLE-CABLE-SERIAL-SPEC.md'
    'docs/LICENSE-nlohmann-json.txt'='docs/LICENSE-nlohmann-json.txt'
}
foreach($taskInput in $taskMapping.Keys){if(-not(Test-Path -LiteralPath (Join-Path $PSScriptRoot $taskInput) -PathType Leaf)){throw ('Missing package input: '+$taskInput)}}
foreach($taskInput in $taskMapping.Keys){$taskTarget=Join-Path $taskStage $taskMapping[$taskInput];New-Item -ItemType Directory -Path (Split-Path $taskTarget) -Force | Out-Null;Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskInput) -Destination $taskTarget}
$taskManifest=@(Get-ChildItem -LiteralPath $taskStage -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($taskStage.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant();bytes=$_.Length}
})
[ordered]@{version=$Version;development_snapshot='v55-common-credit';ymir_included=$false;personal_data_included=$false;files=$taskManifest} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskStage 'manifest.json') -Encoding UTF8
Compress-Archive -LiteralPath $taskStage -DestinationPath $taskZip
$taskChecksum=(Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash.ToLowerInvariant()+'  '+(Split-Path $taskZip -Leaf)
$taskChecksum | Set-Content -LiteralPath (Join-Path (Split-Path $taskStage) 'SHA256SUMS.txt') -Encoding ASCII
Write-Output ('RELEASE_PACKAGE '+$taskZip)
Write-Output $taskChecksum
