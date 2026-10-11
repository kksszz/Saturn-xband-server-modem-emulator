param([Parameter(Mandatory=$true)][string]$SourceDirectory)
$ErrorActionPreference='Stop'
$taskSource=(Resolve-Path -LiteralPath $SourceDirectory).Path
$taskPatch=Join-Path $PSScriptRoot 'mednafen-1.32.1-link-cable.patch'
$taskModemPatch=Join-Path $PSScriptRoot 'mednafen-1.32.1-xband.patch'
$taskCeiling=[Environment]::GetEnvironmentVariable('GIT_CEILING_DIRECTORIES','Process')
[Environment]::SetEnvironmentVariable('GIT_CEILING_DIRECTORIES',(Split-Path $taskSource),'Process')
Push-Location $taskSource
try {
    & git apply --reverse --check -- $taskPatch 2>$null
    if($LASTEXITCODE -eq 0){Write-Output 'Mednafen link-cable integration already applied';return}
    & git apply --reverse --check -- $taskModemPatch 2>$null
    if($LASTEXITCODE -ne 0){throw 'Apply the verified Mednafen 1.32.1 XBAND base integration first; source preserved'}
    $taskBaseline=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'link-cable-baseline-files.json') -Raw | ConvertFrom-Json
    foreach($taskEntry in $taskBaseline.files.PSObject.Properties){
        $taskFile=Join-Path $taskSource $taskEntry.Name
        if(-not(Test-Path -LiteralPath $taskFile -PathType Leaf)){throw ('Missing source: '+$taskEntry.Name)}
        if((Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskEntry.Value){
            throw ('Modified or unsupported cable baseline; preserved: '+$taskEntry.Name)
        }
    }
    & git apply --check -- $taskPatch
    if($LASTEXITCODE -ne 0){throw 'Link-cable preflight failed; source preserved'}
    & git apply -- $taskPatch
    if($LASTEXITCODE -ne 0){throw 'Link-cable patch failed'}
    & git apply --reverse --check -- $taskPatch
    if($LASTEXITCODE -ne 0){throw 'Link-cable postflight failed'}
    Write-Output 'Applied optional link-cable integration; rebuild before enabling'
} finally {Pop-Location;[Environment]::SetEnvironmentVariable('GIT_CEILING_DIRECTORIES',$taskCeiling,'Process')}
