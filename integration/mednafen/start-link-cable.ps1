param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$Disc,
    [Parameter(Mandatory=$true)][string]$JapaneseBios,
    [Parameter(Mandatory=$true)][string]$ProfileDirectory,
    [ValidateRange(1024,65535)][int]$Port=32458,
    [ValidateSet(0,1)][int]$Synchronize=1
)
$ErrorActionPreference='Stop'
$taskExe=(Resolve-Path -LiteralPath $Executable).Path
$taskDisc=(Resolve-Path -LiteralPath $Disc).Path
$taskBios=(Resolve-Path -LiteralPath $JapaneseBios).Path
New-Item -ItemType Directory -Path $ProfileDirectory -Force | Out-Null
$taskProfile=(Resolve-Path -LiteralPath $ProfileDirectory).Path
$taskValues=@{
    MEDNAFEN_HOME=$taskProfile
    XBAND_MEDNAFEN_CABLE='1'
    XBAND_MEDNAFEN_CABLE_PORT=[string]$Port
    XBAND_MEDNAFEN_CABLE_SYNC=[string]$Synchronize
    XBAND_MEDNAFEN_SERVICE_PORT=$null
    XBAND_MEDNAFEN_SUBSCRIBER=$null
    XBAND_MEDNAFEN_CARD=$null
}
$taskSaved=@{}
try{
    foreach($taskKey in $taskValues.Keys){
        $taskSaved[$taskKey]=[Environment]::GetEnvironmentVariable($taskKey,'Process')
        [Environment]::SetEnvironmentVariable($taskKey,$taskValues[$taskKey],'Process')
    }
    # Interactive game window intentionally requested by the launcher.
    $taskArguments=@('-ss.cart','auto','-ss.bios_jp',('"'+$taskBios+'"'),('"'+$taskDisc+'"'))
    $taskProcess=Start-Process -FilePath $taskExe -ArgumentList $taskArguments -WorkingDirectory (Split-Path $taskExe) -WindowStyle Hidden -PassThru
    Write-Output ('Link-cable Mednafen started; PID '+$taskProcess.Id+'; profile '+$taskProfile)
}finally{
    foreach($taskKey in $taskSaved.Keys){[Environment]::SetEnvironmentVariable($taskKey,$taskSaved[$taskKey],'Process')}
}
