param([string]$Version='v0.5.0',[string]$OutputDirectory='artifacts',[string]$PackageRevision)
$ErrorActionPreference='Stop'
if($Version -notmatch '^v\d+\.\d+\.\d+([.-][a-zA-Z0-9.-]+)?$'){throw 'Invalid release version'}
if($PackageRevision -and $PackageRevision -notmatch '^\d{4}-\d{2}-\d{2}$'){throw 'Invalid package revision'}
$taskSourceCommit=(git -C $PSScriptRoot rev-parse HEAD).Trim()
if($LASTEXITCODE -ne 0 -or $taskSourceCommit -notmatch '^[0-9a-f]{40}$'){throw 'Cannot identify package source commit'}
$taskStage=Join-Path (Join-Path $PSScriptRoot $OutputDirectory) ($Version+'/Saturn-xband-server-modem-emulator-windows-x64')
$taskZip=Join-Path (Split-Path $taskStage) ('Saturn-xband-server-modem-emulator-'+$Version+'-windows-x64.zip')
if((Test-Path -LiteralPath $taskStage) -or (Test-Path -LiteralPath $taskZip)){throw 'Preserve existing files; use a fresh OutputDirectory or version'}
$taskMapping=@{
    'build/Release/xband-server.exe'='bin/xband-server.exe'
    'start-server.ps1'='start-server.ps1'
    'README.md'='README.md'
    'RELEASE-NOTES.md'='RELEASE-NOTES.md'
    'THIRD-PARTY-NOTICES.md'='THIRD-PARTY-NOTICES.md'
    'LICENSE'='LICENSE'
    'CMakeLists.txt'='CMakeLists.txt'
    'package-release.ps1'='package-release.ps1'
    'verify-package.ps1'='verify-package.ps1'
    'PACKAGE-CONTENTS.md'='PACKAGE-CONTENTS.md'
    'docs/RELEASE-VERIFICATION.md'='docs/RELEASE-VERIFICATION.md'
    'docs/SERVICE-ENDPOINT-RENAME-VERIFICATION.md'='docs/SERVICE-ENDPOINT-RENAME-VERIFICATION.md'
    'docs/PEER-ROUTING-CATALOG-VERIFICATION.md'='docs/PEER-ROUTING-CATALOG-VERIFICATION.md'
    'config/jp-area-codes.json'='config/jp-area-codes.json'
    'docs/XBAND-COMMAND-SPEC-DRAFT.md'='docs/XBAND-COMMAND-SPEC-DRAFT.md'
    'docs/SERVER-MODEM-SPEC.md'='docs/SERVER-MODEM-SPEC.md'
    'docs/SERVER-UI-MANUAL.md'='docs/SERVER-UI-MANUAL.md'
    'docs/EMULATOR-INTEGRATION-SPEC.md'='docs/EMULATOR-INTEGRATION-SPEC.md'
    'docs/YMIR-INTEGRATION-MANUAL.md'='docs/YMIR-INTEGRATION-MANUAL.md'
    'docs/MEDNAFEN-INTEGRATION-MANUAL.md'='docs/MEDNAFEN-INTEGRATION-MANUAL.md'
    'docs/images/system-architecture.svg'='docs/images/system-architecture.svg'
    'docs/BATTLE-CABLE-SERIAL-SPEC.md'='docs/BATTLE-CABLE-SERIAL-SPEC.md'
    'docs/LICENSE-nlohmann-json.txt'='docs/LICENSE-nlohmann-json.txt'
    'integration/ymir/README.md'='integration/ymir/README.md'
    'integration/ymir/VERIFICATION.md'='integration/ymir/VERIFICATION.md'
    'integration/ymir/ymir-9a237ea-serial-xband.patch'='integration/ymir/ymir-9a237ea-serial-xband.patch'
    'integration/ymir/apply-integration.ps1'='integration/ymir/apply-integration.ps1'
    'integration/ymir/build-ymir.ps1'='integration/ymir/build-ymir.ps1'
    'integration/ymir/LICENSE-GPL-3.0.txt'='integration/ymir/LICENSE-GPL-3.0.txt'
}
foreach($taskComponent in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'components/xband') -File -Recurse){
    if($taskComponent.Extension -notin @('.h','.hpp','.cpp','.inc','.md','.json') -and $taskComponent.Name -ne 'CMakeLists.txt'){continue}
    $taskComponentRelative=$taskComponent.FullName.Substring($PSScriptRoot.Length+1).Replace('\','/')
    if($taskComponentRelative -notmatch '^components/xband/(include/|adapters/|tests/|tools/|[^/]+\.md$|CMakeLists\.txt$)'){continue}
    if($taskComponent.Extension -eq '.json' -and $taskComponentRelative -ne 'components/xband/tests/fixtures/server-config.json'){continue}
    $taskMapping[$taskComponentRelative]=$taskComponentRelative
}
foreach($taskIntegration in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'integration/mednafen') -File){
    if($taskIntegration.Extension -notin @('.ps1','.sh','.json','.patch','.md')){continue}
    $taskRelative='integration/mednafen/'+$taskIntegration.Name
    $taskMapping[$taskRelative]=$taskRelative
}
$taskMapping['docs/images/vf-remix-xband-mednafen.png']='docs/images/vf-remix-xband-mednafen.png'
foreach($taskCable in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'components/link-cable') -File){
    if($taskCable.Extension -notin @('.h','.hpp','.cpp','.inc','.md') -and $taskCable.Name -ne 'CMakeLists.txt'){continue}
    $taskRelative='components/link-cable/'+$taskCable.Name
    $taskMapping[$taskRelative]=$taskRelative
}
$taskMapping['config/service-credit-defaults.json']='config/service-credit-defaults.json'
foreach($taskSource in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'outputs/live-capture-tests') -File){
    # Historical directory name; include code only, never captured traffic/data.
    if($taskSource.Extension -notin @('.h','.hpp','.cpp','.inc')){continue}
    $taskRelative='outputs/live-capture-tests/'+$taskSource.Name
    $taskMapping[$taskRelative]=$taskRelative
}
foreach($taskInput in $taskMapping.Keys){if(-not(Test-Path -LiteralPath (Join-Path $PSScriptRoot $taskInput) -PathType Leaf)){throw ('Missing package input: '+$taskInput)}}
foreach($taskInput in $taskMapping.Keys){$taskTarget=Join-Path $taskStage $taskMapping[$taskInput];New-Item -ItemType Directory -Path (Split-Path $taskTarget) -Force | Out-Null;Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskInput) -Destination $taskTarget}
$taskManifest=@(Get-ChildItem -LiteralPath $taskStage -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($taskStage.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant();bytes=$_.Length}
})
[ordered]@{version=$Version;package_revision=$PackageRevision;source_commit=$taskSourceCommit;created_at_utc=[DateTime]::UtcNow.ToString('o');package_kind='public-prerelease';license='GPL-3.0-only';server_source_included=$true;ymir_included=$false;mednafen_included=$false;emulator_integration_kits=@('ymir','mednafen');features=@('xband','serial-link-cable','communication-log-viewer');ymir_baseline='9a237ea6642912ae0833f691809aa47dc27f908d';mednafen_baseline='1.32.1';personal_data_included=$false;files=$taskManifest} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskStage 'manifest.json') -Encoding UTF8
Compress-Archive -LiteralPath $taskStage -DestinationPath $taskZip
& (Join-Path $PSScriptRoot 'verify-package.ps1') -Archive $taskZip -ExpectedVersion $Version
$taskChecksum=(Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash.ToLowerInvariant()+'  '+(Split-Path $taskZip -Leaf)
$taskChecksum | Set-Content -LiteralPath (Join-Path (Split-Path $taskStage) 'SHA256SUMS.txt') -Encoding ASCII
Write-Output ('RELEASE_PACKAGE '+$taskZip)
Write-Output $taskChecksum
