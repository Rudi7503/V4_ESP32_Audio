# monitor_fenster.ps1 - eigenes Fenster, das den ESP32 live anzeigt.
#
# Dieses Skript ist fuer ein EIGENES Konsolenfenster gedacht (siehe
# monitor_starten.bat). Es zeigt die serielle Ausgabe fortlaufend an, ohne
# Zeitlimit, bis das Fenster mit Strg+C oder Schliessen beendet wird.
#
# Zusaetzlich koennen Kommandos geschickt werden, die dann im selben Fenster
# sichtbar durchlaufen.

param(
    [string]$Port = 'COM3',
    [int]$Baud = 115200,
    [string]$Commands = '',
    [int]$GapSeconds = 15,
    [string]$Log = 'D:\Coding\ESP-IDF\.tmp\logs\monitor_fenster.log'
)

$ErrorActionPreference = 'Continue'

# ---------------------------------------------------------------------------
# Sperre: es darf nur EIN Monitor laufen. Der serielle Port kann nur von einem
# Prozess geoeffnet werden, ein zweites Fenster wuerde nur "Zugriff auf COM3
# verweigert" zeigen. Ein benannter Mutex ist die zuverlaessige Sperre - eine
# Pruefung auf laufende Prozesse greift zu spaet, wenn mehrere gleichzeitig
# starten (das ist genau passiert: drei Fenster, nur eines bekam den Port).
# ---------------------------------------------------------------------------
$createdNew = $false
$mutex = New-Object System.Threading.Mutex($true, 'Global\ESP32_Monitor_COM3', [ref]$createdNew)
if (-not $createdNew) {
    Write-Host ""
    Write-Host "  Es laeuft bereits ein Monitor-Fenster fuer COM3." -ForegroundColor Yellow
    Write-Host "  Bitte dort Strg+C druecken oder das Fenster schliessen," -ForegroundColor Yellow
    Write-Host "  danach dieses hier erneut starten." -ForegroundColor Yellow
    Write-Host ""
    Read-Host "  Enter zum Schliessen"
    exit 1
}

if (-not (Test-Path (Split-Path $Log))) { New-Item -ItemType Directory -Path (Split-Path $Log) -Force | Out-Null }
# Sofort anlegen, nicht erst beim ersten Datenbyte: sonst ist von aussen nicht
# erkennbar, ob der Monitor ueberhaupt laeuft.
Add-Content -LiteralPath $Log -Value "===== Monitor gestartet $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') auf $Port ====="

function Show([string]$text) {
    if (-not $text) { return }
    Write-Host -NoNewline $text
    Add-Content -LiteralPath $Log -Value $text
}

# ---------------------------------------------------------------------------
# Kommando-Kanal: der serielle Port kann nur von EINEM Prozess geoeffnet
# werden. Wenn dieses Fenster ihn haelt, kann kein anderes Skript Kommandos
# senden ("Zugriff auf COM3 verweigert"). Deshalb liest der Monitor eine
# Datei ab: schreibt jemand eine Zeile hinein, wird sie an den ESP32
# geschickt und danach die Datei geleert. So bleiben Anzeige und Steuerung im
# selben Prozess.
# ---------------------------------------------------------------------------
$CmdFile = Join-Path (Split-Path $Log) 'monitor_cmd.txt'

function SendPendingCommands {
    if (-not (Test-Path $CmdFile)) { return }
    if (-not ($script:sp -and $script:sp.IsOpen)) { return }
    $lines = @()
    try { $lines = Get-Content -LiteralPath $CmdFile -ErrorAction Stop } catch { return }
    if (-not $lines) { return }
    # Datei sofort leeren, damit nichts doppelt gesendet wird.
    try { Set-Content -LiteralPath $CmdFile -Value '' -ErrorAction Stop } catch { }
    foreach ($line in $lines) {
        $cmd = "$line".Trim()
        if ($cmd -eq '') { continue }
        Write-Host ""
        Write-Host ">>> $cmd" -ForegroundColor Green
        Add-Content -LiteralPath $Log -Value "`r`n>>> $cmd"
        try { $script:sp.Write($cmd + "`r`n") } catch { }
        # Kurz warten, damit die Antwort im Fluss bleibt.
        $deadline = (Get-Date).AddSeconds(2)
        while ((Get-Date) -lt $deadline) {
            try { $c = $script:sp.ReadExisting(); if ($c) { Show $c } } catch { return }
            Start-Sleep -Milliseconds 50
        }
    }
}

function Pump([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        SendPendingCommands
        try { $c = $script:sp.ReadExisting(); if ($c) { Show $c } }
        catch {
            # Port weg (ESP32 abgezogen): sofort raus, Wait-Port verbindet neu.
            return
        }
        Start-Sleep -Milliseconds 40
    }
}

$script:sp = $null

function Open-Port {
    # Oeffnet den Port. Gibt $true zurueck, wenn er danach offen ist.
    try { $script:sp.Open(); return $true } catch { return $false }
}

# ---------------------------------------------------------------------------
# Wiederverbinden nach Trennung.
#
# WARUM DAS NOETIG IST: der serielle Anschluss haengt am USB des ESP32. Wird
# der Chip zum Neustart kurz ab- und wieder angesteckt (das ist hier der Reset,
# der EN-Taster ist im Board verbaut), verschwindet COM3 kurz. Ohne diese
# Schleife blieb das Fenster danach stumm - und der Boot-Log mit der
# Versionsnummer war jedes Mal verloren. Genau daran sind wir mehrfach
# haengen geblieben.
# ---------------------------------------------------------------------------
function Wait-Port {
    if ($script:sp -and $script:sp.IsOpen) { return $true }
    Write-Host ""
    Write-Host "  $Port ist getrennt - warte auf den ESP32 ..." -ForegroundColor Yellow
    Add-Content -LiteralPath $Log -Value "`r`n--- $Port getrennt, warte auf Neustart ---"
    for ($i = 1; $i -le 600; $i++) {          # bis zu 5 Minuten
        Start-Sleep -Milliseconds 500
        try {
            if ($script:sp) { $script:sp.Dispose() }
        } catch { }
        $script:sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
        $script:sp.ReadTimeout = 200
        $script:sp.DtrEnable = $false
        $script:sp.RtsEnable = $false
        if (Open-Port) {
            Write-Host ""
            Write-Host "  $Port ist wieder da - lese weiter." -ForegroundColor Green
            Add-Content -LiteralPath $Log -Value "`r`n--- $Port wieder verbunden ---"
            return $true
        }
    }
    Write-Host "  $Port kam nicht zurueck." -ForegroundColor Red
    return $false
}

if (-not (Open-Port)) {
    Write-Host ""
    Write-Host "  Port $Port liess sich nicht oeffnen - warte darauf ..." -ForegroundColor Yellow
}

Write-Host "================================================================" -ForegroundColor Cyan
Write-Host " ESP32-Monitor   Port $Port   $Baud Baud" -ForegroundColor Cyan
Write-Host " Beenden: Strg+C  oder Fenster schliessen" -ForegroundColor Cyan
Write-Host " Uebersteht Ab-/Anstecken des ESP32 (verbindet sich neu)" -ForegroundColor Cyan
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""

if ($Commands) {
    foreach ($cmd in ($Commands -split '\s*\|\s*' | Where-Object { $_ -ne '' })) {
        if (-not (Wait-Port)) { break }
        Write-Host ""
        Write-Host ">>> $cmd" -ForegroundColor Green
        Add-Content -LiteralPath $Log -Value "`r`n>>> $cmd"
        try { $sp.Write($cmd + "`r`n") } catch { }
        Pump $GapSeconds
    }
    Write-Host ""
    Write-Host "--- Kommandos abgeschickt, Monitor laeuft weiter (Strg+C beendet) ---" -ForegroundColor Yellow
}

while ($true) {
    if (-not (Wait-Port)) { break }
    Pump 1
}
try { $sp.Close() } catch { }

