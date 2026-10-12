param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$Disc,
    [Parameter(Mandatory=$true)][string]$JapaneseBios,
    [Parameter(Mandatory=$true)][string]$ProfileDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9]+$')][string]$Subscriber,
    [ValidateSet(0,1)][int]$Side=0,
    [ValidateRange(1024,65529)][int]$BasePort=58240
)
$ErrorActionPreference='Stop'
$taskExe=(Resolve-Path -LiteralPath $Executable).Path
$taskDisc=(Resolve-Path -LiteralPath $Disc).Path
$taskBios=(Resolve-Path -LiteralPath $JapaneseBios).Path
New-Item -ItemType Directory -Path $ProfileDirectory -Force | Out-Null
$taskProfile=(Resolve-Path -LiteralPath $ProfileDirectory).Path
$taskValues=@{
    MEDNAFEN_HOME=$taskProfile
    XBAND_MEDNAFEN_CARD=(Join-Path $taskProfile 'cards/synthetic-100.bin')
    XBAND_MEDNAFEN_SERVICE_PORT=[string]($BasePort+$Side)
    XBAND_MEDNAFEN_SUBSCRIBER=$Subscriber
    XBAND_MEDNAFEN_SIDE=[string]$Side
    XBAND_MEDNAFEN_CABLE='0'
}
$taskSaved=@{}
try {
    foreach($taskKey in $taskValues.Keys){$taskSaved[$taskKey]=[Environment]::GetEnvironmentVariable($taskKey,'Process');[Environment]::SetEnvironmentVariable($taskKey,$taskValues[$taskKey],'Process')}
    # Interactive game window is intentional; users perform game operations.
    $taskArguments=@('-ss.cart','xband-probe','-ss.bios_jp',('"'+$taskBios+'"'),'-sound','1',('"'+$taskDisc+'"'))
    $taskProcess=Start-Process -FilePath $taskExe -ArgumentList $taskArguments -WorkingDirectory (Split-Path $taskExe) -PassThru
    Write-Output ('Mednafen started; PID '+$taskProcess.Id+'; side '+$Side+'; profile '+$taskProfile)
} finally {
    foreach($taskKey in $taskValues.Keys){[Environment]::SetEnvironmentVariable($taskKey,$taskSaved[$taskKey],'Process')}
}
