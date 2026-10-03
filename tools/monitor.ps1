<#
    monitor.ps1 - einfacher serieller Monitor fuer den ESP32 (kein IDF-Environment noetig)

    Aufruf (die Execution-Policy dieses Rechners blockiert .ps1, daher so starten):

      powershell -ExecutionPolicy Bypass -File D:\programmier_ordner\ESP-IDF\.tmp\monitor.ps1

    Optionen:
      -Port COM4               anderer Port (Vorgabe COM3)
      -Baud 115200             andere Baudrate
      -Filter 'bt_mgr|sd_fs'   nur Zeilen ausgeben, die auf den Regex passen
      -Reset                   vorher einen EN-Puls geben (sauberer Boot-Log)
      -Duration 30             nach 30 s automatisch beenden (0 = bis Strg+C)
      -LogFile C:\...\x.log    zusaetzlich ungefiltert in eine Datei schreiben

    Beenden mit Strg+C.

    Hinweis: Der Port ist exklusiv. Wenn VS Code oder idf.py monitor laeuft,
    schlaegt das Oeffnen mit "Zugriff verweigert" fehl - das andere Programm
    zuerst schliessen.
#>
param(
    [string]$Port = 'COM3',
    [int]$Baud = 115200,
    [string]$Filter = '',
    [switch]$Reset,
    [int]$Duration = 0,
    [string]$LogFile = ''
)

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 200

try {
    $sp.Open()
} catch {
    Write-Host "Port $Port liess sich nicht oeffnen: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Laeuft noch ein Monitor in VS Code oder idf.py? Der Port ist exklusiv." -ForegroundColor Yellow
    exit 1
}

try {
    # DTR bleibt aus (GPIO0 high), RTS nur fuer den optionalen Reset pulsen
    $sp.DtrEnable = $false
    if ($Reset) {
        $sp.RtsEnable = $true
        Start-Sleep -Milliseconds 150
        $sp.RtsEnable = $false
    } else {
        $sp.RtsEnable = $false
    }

    Write-Host "Monitor auf $Port @ $Baud" -NoNewline -ForegroundColor Cyan
    if ($Filter -ne '') { Write-Host "  Filter: $Filter" -NoNewline -ForegroundColor Cyan }
    if ($LogFile -ne '') { Write-Host "  Log: $LogFile" -NoNewline -ForegroundColor Cyan }
    Write-Host "  (Ende mit Strg+C)" -ForegroundColor Cyan
    Write-Host ""

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $buf = New-Object System.Text.StringBuilder

    while ($true) {
        $chunk = $sp.ReadExisting()
        if ($chunk.Length -gt 0) {
            [void]$buf.Append($chunk)

            # nur vollstaendige Zeilen ausgeben, den Rest puffern
            $txt = $buf.ToString()
            $cut = $txt.LastIndexOf("`n")
            if ($cut -ge 0) {
                $complete = $txt.Substring(0, $cut + 1)
                [void]$buf.Clear()
                [void]$buf.Append($txt.Substring($cut + 1))

                foreach ($line in ($complete -split "`r?`n")) {
                    if ($line.Length -eq 0) { continue }
                    if ($Filter -eq '' -or $line -match $Filter) {
                        # Fehler/Warnungen farblich hervorheben
                        if ($line -match '\sE \(')      { Write-Host $line -ForegroundColor Red }
                        elseif ($line -match '\sW \(')  { Write-Host $line -ForegroundColor Yellow }
                        else                            { Write-Host $line }
                    }
                    if ($LogFile -ne '') { Add-Content -Path $LogFile -Value $line }
                }
            }
        }

        Start-Sleep -Milliseconds 40

        if ($Duration -gt 0 -and $sw.Elapsed.TotalSeconds -ge $Duration) {
            Write-Host ""
            Write-Host "($Duration s erreicht, Monitor beendet)" -ForegroundColor Cyan
            break
        }
    }
} finally {
    if ($sp.IsOpen) {
        $sp.DtrEnable = $false
        $sp.RtsEnable = $false
        $sp.Close()
    }
}
