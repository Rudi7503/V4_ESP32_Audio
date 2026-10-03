# flash_only.ps1 - flasht nur und wartet danach auf einen Neustart von Hand.
#
# Wichtig: nach dem Flashen mit --after no-reset bleibt der Chip im Bootloader
# stehen. Ein Reset per esptool hilft dort NICHT, weil esptool den Chip dazu
# wieder im Download-Modus bräuchte - er bekommt aber keine Antwort mehr. Der
# einzige zuverlaessige Weg ist EN/Reset von Hand (oder kurz stromlos).
#
# Deshalb: flashen, dann laufen lesen und den Anwender tippen lassen. Das
# Skript zeigt den Boot-Log live an und beendet sich nach der Lesezeit.
#
# Aufruf:
#   powershell -ExecutionPolicy Bypass -File .tmp\flash_only.ps1 [-ReadSeconds 40]

param(
    [string]$Port = '',
    [int]$Baud = 460800,
    [int]$ReadBaud = 115200,
    [int]$MaxAttempts = 8,
    [int]$ReadSeconds = 40,
    [switch]$SkipFlash,
    [string]$Log = 'D:\Coding\ESP-IDF\.tmp\logs\flash_only.log'
)

$ErrorActionPreference = 'Continue'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
$env:IDF_PATH = 'D:\Coding\ESP-IDF\.espressif\v6.1\esp-idf'
. 'D:\Coding\ESP-IDF\.tmp\idf61_env.ps1'

# Port aus port.txt, falls keiner uebergeben wurde: der COM-Anschluss wechselt
# je nach USB-Buchse oder nach einem Neustart des Rechners (zuletzt COM3 -> COM7).
if ([string]::IsNullOrWhiteSpace($Port)) {
    $portFile = 'D:\Coding\ESP-IDF\.tmp\port.txt'
    if (Test-Path $portFile) {
        $Port = (Get-Content $portFile -Raw).Trim()
    }
    if ([string]::IsNullOrWhiteSpace($Port)) { $Port = 'COM7' }
}
Write-Host "Port: $Port"

$build = 'D:\Coding\ESP-IDF\V4_ESP32\build'
if (-not (Test-Path (Split-Path $Log))) { New-Item -ItemType Directory -Path (Split-Path $Log) -Force | Out-Null }
# NICHT leeren! Diese Datei ist gleichzeitig die Quelle fuer ein eventuell
# offenes Flash-Fenster (flash_fenster.ps1 liest den Zuwachs mit). Ein
# Set-Content hier hat die Anzeige im Fenster bisher leer gewischt.
Add-Content -LiteralPath $Log -Value "`r`n===== Start $(Get-Date -Format 'HH:mm:ss') ====="
$script:log = $Log
function Echo([string]$t) { Write-Host $t; Add-Content -LiteralPath $script:log -Value $t }

if (-not $SkipFlash) {
    $flashArgs = ((Get-Content (Join-Path $build 'flash_args')) | Where-Object { $_ -and $_ -notmatch '^\s*#' }) -join ' '
    $esptoolArgs = @('-m', 'esptool', '--chip', 'esp32', '-p', $Port, '-b', "$Baud",
                     '--before', 'no-reset', '--after', 'no-reset', 'write-flash') +
                   ($flashArgs -split '\s+' | Where-Object { $_ })
    Echo "===== FLASHEN auf $Port ====="
    Echo "Download-Modus: BOOT halten, EN tippen, BOOT loslassen."
    $exit = 1
    for ($try = 1; $try -le $MaxAttempts; $try++) {
        Echo "--- Versuch $try/$MaxAttempts ---"
        Push-Location $build
        & python @esptoolArgs 2>&1 | ForEach-Object { Echo $_ }
        $exit = $LASTEXITCODE
        Pop-Location
        if ($exit -eq 0) { break }
        Echo "(noch nicht im Download-Modus)"
        Start-Sleep -Seconds 4
    }
    Echo "===== flash exit=$exit ====="
    if ($exit -ne 0) { Echo "ABBRUCH"; exit $exit }
}

# Phase melden, BEVOR der Port geoeffnet wird: ein eventuell laufendes
# Flash-/Monitor-Fenster sieht das und gibt COM3 frei. Danach darauf warten -
# sonst blockieren sich die beiden Prozesse gegenseitig.
Set-Content -LiteralPath 'D:\Coding\ESP-IDF\.tmp\logs\flash_phase.txt' -Value 'READING'

$sp = $null
for ($try = 1; $try -le 20; $try++) {
    try {
        $sp = New-Object System.IO.Ports.SerialPort($Port, $ReadBaud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
        $sp.ReadTimeout = 300
        $sp.DtrEnable = $false
        $sp.RtsEnable = $false
        $sp.Open()
        break
    } catch {
        $sp = $null
        if ($try -eq 20) { Echo "Port $Port nicht oeffenbar: $($_.Exception.Message)"; exit 1 }
        Start-Sleep -Milliseconds 500
    }
}
if ($sp -eq $null) { Echo "Port $Port nicht verfuegbar"; exit 1 }

Echo ""
Echo "================================================================"
Echo " JETZT EN/Reset am Modul kurz auf GND tippen (oder stromlos)!"
Echo " Der Chip steht im Bootloader und startet nur so neu."
Echo " Lesezeit: $ReadSeconds s ab jetzt."
Echo "================================================================"

$deadline = (Get-Date).AddSeconds($ReadSeconds)
while ((Get-Date) -lt $deadline) {
    try { $c = $sp.ReadExisting(); if ($c) { Write-Host -NoNewline $c; Add-Content -LiteralPath $script:log -Value $c } } catch { }
    Start-Sleep -Milliseconds 100
}
$sp.Close()
Echo ""
Echo "===== ENDE $(Get-Date -Format 'HH:mm:ss') ====="
exit 0
