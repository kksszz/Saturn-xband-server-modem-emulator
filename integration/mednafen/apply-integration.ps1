param([Parameter(Mandatory=$true)][string]$SourceDirectory)
$ErrorActionPreference='Stop'
$taskSource=(Resolve-Path -LiteralPath $SourceDirectory).Path
$taskPatch=Join-Path $PSScriptRoot 'mednafen-1.32.1-xband.patch'
$taskBaseline=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'baseline-files.json') -Raw | ConvertFrom-Json
Push-Location $taskSource
$taskCeiling=[Environment]::GetEnvironmentVariable('GIT_CEILING_DIRECTORIES','Process')
[Environment]::SetEnvironmentVariable('GIT_CEILING_DIRECTORIES',(Split-Path $taskSource),'Process')
try {
    $taskCablePatch=Join-Path $PSScriptRoot 'mednafen-1.32.1-link-cable.patch'
    if(Test-Path -LiteralPath $taskCablePatch){
        & git apply --reverse --check -- $taskCablePatch 2>$null
        if($LASTEXITCODE -eq 0){Write-Output 'Mednafen XBAND base with optional link-cable integration already applied';return}
    }
    & git apply --reverse --check -- $taskPatch 2>$null
    if($LASTEXITCODE -eq 0){Write-Output 'Mednafen XBAND integration already applied';return}
    foreach($taskEntry in $taskBaseline.files.PSObject.Properties){
        $taskFile=Join-Path $taskSource $taskEntry.Name
        if(-not(Test-Path -LiteralPath $taskFile -PathType Leaf)){throw ('Missing upstream file: '+$taskEntry.Name)}
        if((Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskEntry.Value){throw ('Modified or unsupported upstream source; preserved: '+$taskEntry.Name)}
    }
    & git apply --check -- $taskPatch
    if($LASTEXITCODE -ne 0){throw 'Integration preflight failed; source preserved'}
    & git apply -- $taskPatch
    if($LASTEXITCODE -ne 0){throw 'Integration apply failed'}
    & git apply --reverse --check -- $taskPatch
    if($LASTEXITCODE -ne 0){throw 'Integration postflight failed'}
    Write-Output 'Applied XBAND integration to verified Mednafen 1.32.1 source'
} finally {[Environment]::SetEnvironmentVariable('GIT_CEILING_DIRECTORIES',$taskCeiling,'Process');Pop-Location}
