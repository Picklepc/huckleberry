# Registers a Scheduled Task that runs the collector loop at logon.
# Run from an elevated PowerShell:  .\register-task.ps1
# Remove with:  Unregister-ScheduledTask -TaskName VictronCollector -Confirm:$false

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here 'collector.py'

# Prefer pythonw.exe (no console window) if available, else python.exe
$py = (Get-Command pythonw.exe -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = (Get-Command python.exe -ErrorAction Stop).Source }

if (-not (Test-Path (Join-Path $here 'config.json'))) {
    Write-Warning "config.json not found. Copy config.example.json to config.json and edit it first."
}

$action  = New-ScheduledTaskAction  -Execute $py -Argument "`"$script`"" -WorkingDirectory $here
$trigger = New-ScheduledTaskTrigger  -AtLogOn
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
              -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)

Register-ScheduledTask -TaskName 'VictronCollector' -Action $action -Trigger $trigger `
    -Settings $settings -Description 'Polls ESP32 Victron devices into SQL Server Express' -Force

Write-Host "Registered scheduled task 'VictronCollector' (runs collector loop at logon)."
Write-Host "Start it now with:  Start-ScheduledTask -TaskName VictronCollector"
