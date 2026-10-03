# mess_bauen.ps1 - baut das Projekt fuer einen der Messfaelle.
#
# Aufruf:
#   powershell -ExecutionPolicy Bypass -File .tmp\mess_bauen.ps1 -Mode 0   # nur MP3
#   powershell -ExecutionPolicy Bypass -File .tmp\mess_bauen.ps1 -Mode 1   # I2S mit GMF-Wandler
#   powershell -ExecutionPolicy Bypass -File .tmp\mess_bauen.ps1 -Mode 2   # I2S mit eigenem linearen Wandler
#
# WARUM DER CACHE GELOESCHT WIRD:
# I2S_MODE ist eine CMake-Cache-Variable. Aendert man sie per -D, wuerde CMake
# zwar neu konfigurieren, aber ein alter Wert kann in build.ninja haengen
# bleiben - dann laeuft unbemerkt der falsche Fall. Deshalb wird der Cache hier
# bewusst entfernt.
#
# Die Version in version.txt wird NICHT geaendert: sie ist unser Nachweis,
# welcher Stand auf dem Chip laeuft. Bei jeder Aenderung von Hand hochzaehlen.

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('0', '1', '2', '3', '4')]
    [string]$Mode,

    # Ohne PSRAM bauen: PSRAM in den sdkconfig.defaults abschalten.
    # Noetig, weil das Ersatzmodul keinen PSRAM hat.
    [switch]$NoPsram,

    # Kleinerer GMF-Pool (POOL_SMALL): laesst aud_aec, aud_asrc sowie io_codec_dev
    # und die Pipelines bt2codec/codec2bt weg. Noetig fuer Module ohne PSRAM,
    # weil der vollstaendige Pool-Aufbau dort an "Memory exhausted" scheitert.
    [switch]$Small
)

$ErrorActionPreference = 'Continue'
$proj = 'D:\Coding\ESP-IDF\V4_ESP32'
$build = Join-Path $proj 'build'

$namen = @{
    '0' = 'nur MP3 (kein I2S)'
    '1' = 'I2S mit GMF-Wandlern (rate -> bit)'
    '2' = 'I2S mit eigenem linearen Wandler (Q16.16)'
    '3' = 'I2S: erst 32->16 Bit (GMF-Bitwandler), DANN GMF-Ratenwandlung'
    '4' = 'I2S: erst 32->16 Bit (eigener Shift >>16), DANN GMF-Ratenwandlung'
}

$version = (Get-Content (Join-Path $proj 'version.txt') -Raw).Trim()
Write-Host ""
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host " Messfall $Mode : $($namen[$Mode])"
Write-Host " Version       : $version"
Write-Host " PSRAM         : $(if ($NoPsram) { 'AUS (Modul ohne PSRAM)' } else { 'an' })"
Write-Host " Pool          : $(if ($Small) { 'KLEIN (ohne AEC/ASRC/Codec-Pipelines)' } else { 'vollstaendig' })"
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""

if ($NoPsram) {
    # ---------------------------------------------------------------
    # PSRAM aus: eine Variante von sdkconfig.defaults.esp32 erzeugen.
    #
    # WARUM SO: CMake baut sdkconfig jedes Mal aus sdkconfig.defaults*.neu
    # zusammen. Zeilen in sdkconfig zu loeschen genuegt deshalb nicht - die
    # defaults setzen CONFIG_SPIRAM=y erneut. Also muss die defaults-Datei
    # selbst umgeschaltet werden.
    #
    # Die Varianten werden aus der Originaldatei ERZEUGT (nicht von Hand
    # gepflegt), damit sie nicht auseinanderlaufen. Die Originaldatei wird
    # einmalig als *.psram gesichert und ist damit die Quelle.
    # ---------------------------------------------------------------
    $def = Join-Path $proj 'sdkconfig.defaults.esp32'
    $defPsram = Join-Path $proj 'sdkconfig.defaults.esp32.psram'
    $defNoPsram = Join-Path $proj 'sdkconfig.defaults.esp32.nopsram'

    if (-not (Test-Path $defPsram)) {
        Copy-Item $def $defPsram -Force
        Write-Host "  Original gesichert: sdkconfig.defaults.esp32.psram"
    }

    $lines = Get-Content $defPsram
    $out = New-Object System.Collections.Generic.List[string]
    $entfernt = 0
    foreach ($l in $lines) {
        if ($l -match 'CONFIG_(SPIRAM|ESP32_SPIRAM_SUPPORT|FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)') {
            $entfernt++
            continue
        }
        if ($l -match '^\s*#\s*end of ESP PSRAM') {
            $out.Add('# CONFIG_SPIRAM is not set')
        }
        $out.Add($l)
    }
    Set-Content -LiteralPath $defNoPsram -Value $out
    Copy-Item $defNoPsram $def -Force
    Write-Host "  PSRAM aus: $entfernt Zeilen entfernt, sdkconfig.defaults.esp32 umgeschaltet"
} else {
    # PSRAM an: die gesicherte Originaldatei zurueckholen.
    $def = Join-Path $proj 'sdkconfig.defaults.esp32'
    $defPsram = Join-Path $proj 'sdkconfig.defaults.esp32.psram'
    if (Test-Path $defPsram) {
        Copy-Item $defPsram $def -Force
        Write-Host "  PSRAM an: sdkconfig.defaults.esp32 aus der Sicherung zurueckgeholt"
    }
}

Write-Host "Entferne CMake-Cache, damit I2S_MODE sicher greift ..."
Remove-Item (Join-Path $build 'CMakeCache.txt') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $build 'CMakeFiles') -Recurse -Force -ErrorAction SilentlyContinue

# ---------------------------------------------------------------------------
# sdkconfig MITLOESCHEN.
#
# WARUM DAS NOETIG IST (am 02.10. teuer gelernt): eine vorhandene sdkconfig hat
# Vorrang vor sdkconfig.defaults*. Ohne diese Zeile blieb der PSRAM-Schalter
# wirkungslos - "-NoPsram" und der PSRAM-Build ergaben Bit fuer Bit dasselbe
# Image (beide 2 206 240 Byte), und erst der Blick in build/config/sdkconfig.h
# zeigte, dass CONFIG_SPIRAM weiterhin gesetzt war.
#
# Die sdkconfig ist ein Erzeugnis (aus sdkconfig.defaults* + Menue).
# Handgemachte Werte darin gehen beim Loeschen verloren - dafuer muessen sie in
# sdkconfig.defaults.esp32 stehen. Das ist der Sinn der Datei.
# ---------------------------------------------------------------------------
$cfg = Join-Path $proj 'sdkconfig'
if (Test-Path $cfg) {
    Copy-Item $cfg (Join-Path $proj 'sdkconfig.vorheriger_lauf') -Force
    Remove-Item $cfg -Force
    Write-Host "  sdkconfig geloescht (Sicherung: sdkconfig.vorheriger_lauf)"
}

$suffix = if ($NoPsram) { 'nopsram' } else { 'psram' }
if ($Small) { $suffix = "${suffix}_small" }
$log = "D:\Coding\ESP-IDF\.tmp\logs\mess_${Mode}_${suffix}_build.log"
if (Test-Path $log) { Remove-Item $log -Force }

$poolArg = if ($Small) { '1' } else { '0' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'D:\Coding\ESP-IDF\.tmp\idf61_build.ps1' build "-DI2S_MODE=$Mode" "-DPOOL_SMALL=$poolArg" *> $log
$exit = $LASTEXITCODE

Write-Host ""
Write-Host "=== Ergebnis ==="
Select-String -Path $log -Pattern 'I2S-Modus:|Pool klein|POOL_SMALL|error:|FAILED|Project build complete|binary size' |
    Select-Object -Last 12 | ForEach-Object { $_.Line.Trim() }

if (Test-Path (Join-Path $build 'bt_audio.bin')) {
    $bin = Get-Item (Join-Path $build 'bt_audio.bin')
    Write-Host ""
    Write-Host ("bt_audio.bin: {0} Byte, {1}" -f $bin.Length, $bin.LastWriteTime)
}
Write-Host "build exit=$exit"
exit $exit
