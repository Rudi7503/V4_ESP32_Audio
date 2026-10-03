# idf61_env.ps1 - eingefrorene IDF-v6.1-Umgebung, erzeugt aus
#   idf_tools.py export --format key-value
#
# Warum eingefroren: der DSH-Datei-Sandbox verweigert den Zugriff auf
# esp-clangd\...\bin\clangd.exe. idf_tools.py fuehrt beim Export die
# Versionsabfrage jedes Werkzeugs aus, bekommt dabei WinError 2 und bricht mit
# "tool esp-clangd has no installed versions" ab - die Aktivierung scheitert,
# obwohl das Werkzeug installiert ist. Dieses Skript setzt dieselben Werte
# direkt und laesst die Pruefung aus.
$env:OPENOCD_SCRIPTS = 'C:\Users\Papa\.espressif\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\share\openocd\scripts'
$env:IDF_CCACHE_ENABLE = '1'
$env:ESP_ROM_ELF_DIR = 'C:\Users\Papa\.espressif\tools\esp-rom-elfs\20241011\'
$env:IDF_PYTHON_ENV_PATH = 'C:\Users\Papa\.espressif\python_env\idf6.1_py3.14_env'
$env:ESP_IDF_VERSION = '6.1'
$env:PATH = 'C:\Users\Papa\.espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin;C:\Users\Papa\.espressif\tools\riscv32-esp-elf-gdb\17.1_20260402\riscv32-esp-elf-gdb\bin;C:\Users\Papa\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Users\Papa\.espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Users\Papa\.espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Users\Papa\.espressif\tools\cmake\4.0.3\bin;C:\Users\Papa\.espressif\tools\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\bin;C:\Users\Papa\.espressif\tools\ninja\1.12.1\;C:\Users\Papa\.espressif\tools\idf-exe\1.0.3\;C:\Users\Papa\.espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Users\Papa\.espressif\tools\dfu-util\0.11\dfu-util-0.11-win64;C:\Users\Papa\.espressif\tools\esp-clangd\esp-21.1.3_20260408\esp-clangd\bin;C:\Users\Papa\.espressif\python_env\idf6.1_py3.14_env\Scripts;D:\Coding\ESP-IDF\.espressif\v6.1\esp-idf\tools;' + $env:PATH
$env:IDF_TOOLS_PATH = 'C:\Users\Papa\.espressif'
$env:IDF_PATH = 'D:\Coding\ESP-IDF\.espressif\v6.1\esp-idf'
