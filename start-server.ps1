param([ValidateRange(1024,65530)][int]$BasePort=58240)
$ErrorActionPreference='Stop'
$taskExe=Join-Path $PSScriptRoot 'bin/xband-server.exe'
if(-not(Test-Path -LiteralPath $taskExe)){$taskExe=Join-Path $PSScriptRoot 'build/Release/xband-server.exe'}
if(-not(Test-Path -LiteralPath $taskExe)){throw 'Server executable not found. Extract the Windows release ZIP or build the source.'}
$taskRuntime=Join-Path $PSScriptRoot 'runtime'
New-Item -ItemType Directory -Path $taskRuntime -Force | Out-Null
$taskVariables=@{
    XBAND_TEST_BASE_PORT=[string]$BasePort
    XBAND_REGION_TABLE_FILE=(Join-Path $PSScriptRoot 'config/jp-area-codes.json')
    XBAND_ACTIVITY_HISTORY_DIR=(Join-Path $taskRuntime 'activity-history')
    XBAND_LOCAL_MAIL_JOURNAL_DIR=(Join-Path $taskRuntime 'mail-journal')
    XBAND_GAME_RESULT_DB_DIR=(Join-Path $taskRuntime 'game-result-db')
    XBAND_GAME_RANKING_FILE=(Join-Path $taskRuntime 'game-ranking-settings.json')
    XBAND_GAME_POINT_LEDGER_FILE=(Join-Path $taskRuntime 'game-point-ledger.json')
    XBAND_GAME_LEVEL_FILE=(Join-Path $taskRuntime 'game-level-settings.json')
    XBAND_USAGE_AREA_FILE=(Join-Path $taskRuntime 'usage-area-settings.json')
    XBAND_STANDBY_WAIT_FILE=(Join-Path $taskRuntime 'standby-wait-settings.json')
    XBAND_SERVICE_CREDIT_FILE=(Join-Path $taskRuntime 'service-credit-settings.json')
    XBAND_SERVICE_CREDIT_LEDGER_FILE=(Join-Path $taskRuntime 'service-credit-settlements.json')
    XBAND_MATCH_CREDIT_EPISODES_FILE=(Join-Path $taskRuntime 'match-credit-episodes.json')
    XBAND_STANDBY='1';XBAND_POSTMATCH_EXPERIMENT='0'
}
$taskSaved=@{}
try{
    foreach($taskEntry in Get-ChildItem Env: | Where-Object {$_.Name -like 'XBAND_*'}){
        $taskSaved[$taskEntry.Name]=$taskEntry.Value;[Environment]::SetEnvironmentVariable($taskEntry.Name,$null,'Process')
    }
    foreach($taskKey in $taskVariables.Keys){[Environment]::SetEnvironmentVariable($taskKey,$taskVariables[$taskKey],'Process')}
    $taskServer=Start-Process -FilePath $taskExe -WorkingDirectory $taskRuntime -WindowStyle Hidden -PassThru
    Write-Host ('Server started. PID '+$taskServer.Id+'. Close Communication Monitor to exit. Runtime: '+$taskRuntime)
    $taskServer.WaitForExit()
}finally{
    foreach($taskEntry in Get-ChildItem Env: | Where-Object {$_.Name -like 'XBAND_*'}){[Environment]::SetEnvironmentVariable($taskEntry.Name,$null,'Process')}
    foreach($taskKey in $taskSaved.Keys){[Environment]::SetEnvironmentVariable($taskKey,$taskSaved[$taskKey],'Process')}
}
