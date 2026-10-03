# send_cmd.ps1 - schickt Konsolen-Kommandos an die laufende BTAudio-CLI.
#
# Die CLI des Beispiels haengt an derselben UART wie das Log (115200 Baud), ein
# interaktives Terminal gibt es hier aber nicht: "idf.py monitor" laesst sich
# nicht sauber per Skript beenden. Deshalb dieser Ersatz - Port oeffnen, Zeilen
# senden, feste Zeit mitlesen, alles in die Logdatei.
#
# DTR/RTS bleiben aus: unser Auto-Reset ist nicht verdrahtet, ein gesetztes
# DTR/RTS wuerde nur den Pegel verstelleen.
#
# Aufruf:
#   powershell -ExecutionPolicy Bypass -File .tmp\send_cmd.ps1 -CommandSequence "start_discovery|stop_discovery"
#   powershell -ExecutionPolicy Bypass -File .tmp\send_cmd.ps1 -CommandSequence "start_discovery Headset" -GapSeconds 40

param(
    [string]$Port = 'COM3',
    [int]$Baud = 115200,
    # Kommandos in Reihenfolge, durch | getrennt (nicht durch Komma: bei
    # "powershell -File" wird ein Array-Parameter nicht als Array uebergeben,
    # dann landen die Folgeargumente im naechsten Parameter).
    [string]$CommandSequence = 'help',
    [int]$GapSeconds = 12,
    [int]$TailSeconds = 15,
    [switch]$ResetFirst,
    [string]$Log = 'D:\Coding\ESP-IDF\.tmp\logs\cli.log'
)

$Commands = $CommandSequence -split '\s*\|\s*' | Where-Object { $_ -ne '' }

$sp = New-Object System.IO.Ports.SerialPort($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.ReadTimeout = 500
$sp.DtrEnable = $ResetFirst
$sp.RtsEnable = $ResetFirst
try {
    $sp.Open()
} catch {
    Add-Content -LiteralPath $Log -Value "Port $Port liess sich nicht oeffnen: $($_.Exception.Message)"
    exit 1
}

if ($ResetFirst) {
    # DTR auf EN ist bei unserem Aufbau nicht verdrahtet; der Versuch schadet
    # nicht, aber verlaesslich resettet wird nur von Hand.
    Start-Sleep -Milliseconds 200
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    Start-Sleep -Seconds 3
}

$sb = New-Object System.Text.StringBuilder

function Drain([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $chunk = $sp.ReadExisting()
            if ($chunk) { [void]$sb.Append($chunk) }
        } catch { }
        Start-Sleep -Milliseconds 100
    }
}

Add-Content -LiteralPath $Log -Value "===== CLI-Session auf $Port, Start $(Get-Date -Format 'HH:mm:ss') ====="
Drain 2

foreach ($cmd in $Commands) {
    [void]$sb.Append("`r`n>>> $cmd`r`n")
    $sp.Write($cmd + "`r`n")
    Drain $GapSeconds
    $sp.Write("`r`n")
    Drain 2
}

Drain $TailSeconds
$sp.Close()

Add-Content -LiteralPath $Log -Value $sb.ToString()
Add-Content -LiteralPath $Log -Value "===== Session beendet ($($sb.Length) Zeichen) ====="
exit 0
