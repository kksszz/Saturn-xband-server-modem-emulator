param([Parameter(Mandatory=$true)][string]$Archive,[string]$ExpectedVersion='v0.5.0')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskZip=[IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Archive).Path)
try {
    $taskEntries=@($taskZip.Entries | Where-Object {-not $_.FullName.EndsWith('/')})
    $taskManifestEntries=@($taskEntries | Where-Object {$_.FullName -match '^[^/]+/manifest.json$'})
    if($taskManifestEntries.Count -ne 1){throw 'Expected exactly one root manifest'}
    $taskPrefix=$taskManifestEntries[0].FullName.Substring(0,$taskManifestEntries[0].FullName.Length-'manifest.json'.Length)
    $taskReader=[IO.StreamReader]::new($taskManifestEntries[0].Open())
    try {$taskManifest=$taskReader.ReadToEnd() | ConvertFrom-Json} finally {$taskReader.Dispose()}
    if($taskManifest.version -ne $ExpectedVersion){throw 'Package version mismatch'}
    if(-not $taskManifest.server_source_included -or $taskManifest.ymir_included -or $taskManifest.mednafen_included -or $taskManifest.personal_data_included){throw 'Invalid distribution boundary'}
    $taskNames=@{}
    foreach($taskEntry in $taskEntries){
        if(-not $taskEntry.FullName.StartsWith($taskPrefix,[StringComparison]::Ordinal)){throw 'Multiple package roots'}
        $taskName=$taskEntry.FullName.Substring($taskPrefix.Length)
        if($taskName -match '(^|/)\.\.(/|$)|[:\\]' -or $taskName.StartsWith('/')){throw 'Unsafe archive path'}
        if($taskNames.ContainsKey($taskName)){throw ('Duplicate archive entry: '+$taskName)}
        $taskNames[$taskName]=$taskEntry
        if($taskName -match '(?i)(^|/)(runtime|profiles?|sav|mail-journal|communication-logs|activity-history|game-result-db)(/|$)' -or $taskName -match '(?i)\.(bin|cue|iso|chd|ssz|dll|pdb|log|jsonl|pending)$'){throw ('Forbidden private/emulator input: '+$taskName)}
        if($taskName -match '(?i)\.exe$' -and $taskName -ne 'bin/xband-server.exe'){throw 'Unexpected emulator/tool executable'}
        if($taskName -match '(?i)\.(png|jpg|bmp)$' -and $taskName -ne 'docs/images/vf-remix-xband-mednafen.png'){throw 'Unexpected image asset'}
        if($taskName -match '(?i)\.json$' -and $taskName -notin @('manifest.json','config/jp-area-codes.json','config/service-credit-defaults.json','components/xband/tests/fixtures/server-config.json','integration/mednafen/baseline-files.json','integration/mednafen/link-cable-baseline-files.json')){throw ('Unexpected data file: '+$taskName)}
    }
    if($taskManifest.files.Count+1 -ne $taskEntries.Count){throw 'Unmanifested or missing files'}
    $taskDeclared=@{}
    foreach($taskFile in $taskManifest.files){
        if($taskDeclared.ContainsKey($taskFile.path)){throw 'Duplicate manifest file'}
        $taskDeclared[$taskFile.path]=$true
        if(-not $taskNames.ContainsKey($taskFile.path)){throw ('Missing file: '+$taskFile.path)}
        $taskEntry=$taskNames[$taskFile.path]
        if($taskEntry.Length -ne $taskFile.bytes){throw ('Length mismatch: '+$taskFile.path)}
        $taskStream=$taskEntry.Open();$taskSHA=[Security.Cryptography.SHA256]::Create()
        try {$taskHash=([BitConverter]::ToString($taskSHA.ComputeHash($taskStream))).Replace('-','').ToLowerInvariant()} finally {$taskStream.Dispose();$taskSHA.Dispose()}
        if($taskHash -ne $taskFile.sha256){throw ('Hash mismatch: '+$taskFile.path)}
    }
    foreach($taskRequired in @('bin/xband-server.exe','CMakeLists.txt','LICENSE','PACKAGE-CONTENTS.md','components/xband/CMakeLists.txt','components/xband/include/xband/modem_register_bank.hpp','components/xband/adapters/ymir/frontend_modem.cpp','components/xband/adapters/mednafen/command_runtime.cpp','components/xband/adapters/mednafen/cable_runtime.cpp','components/xband/adapters/mednafen/cable_core_test.inc','components/link-cable/CMakeLists.txt','components/link-cable/sci_state.hpp','components/link-cable/link_cable_service.cpp','components/link-cable/link_cable_sync.inc','outputs/live-capture-tests/pb3_service_server.cpp','docs/SERVER-MODEM-SPEC.md','docs/BATTLE-CABLE-SERIAL-SPEC.md','docs/YMIR-INTEGRATION-MANUAL.md','docs/MEDNAFEN-INTEGRATION-MANUAL.md','docs/EMULATOR-INTEGRATION-SPEC.md','integration/ymir/ymir-9a237ea-serial-xband.patch','integration/mednafen/mednafen-1.32.1-xband.patch','integration/mednafen/mednafen-1.32.1-link-cable.patch','integration/mednafen/start-link-cable.ps1','integration/mednafen/LINK-CABLE.md')){
        if(-not $taskNames.ContainsKey($taskRequired)){throw ('Required release input missing: '+$taskRequired)}
    }
    Write-Output ('Verified '+$taskManifest.files.Count+' files: hashes, required sources/guides, and distribution exclusions')
} finally {$taskZip.Dispose()}
