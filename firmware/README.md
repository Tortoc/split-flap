# Vorkompilierte Firmware

Dieser Ordner enthält die **jeweils aktuellste kompilierte Firmware** für
alle drei Komponenten des Projekts, direkt aus dem Quellcode in diesem Repo
gebaut. Praktisch, wenn kein Arduino-Toolchain-Setup gewünscht ist - für
Entwicklung/Änderungen bleibt der Quellcode in [`ESPMaster/`](../ESPMaster),
[`Unit/`](../Unit) und [`Unit/bootloader/twiboot/`](../Unit/bootloader/twiboot)
maßgeblich.

⚠️ **Diese Dateien werden manuell aktuell gehalten**, nicht automatisch per
CI gebaut. Stand: siehe `BUILD_INFO.md` in diesem Ordner (Commit-Hash +
Datum des letzten Builds). Bei Zweifel: Datum/Commit-Hash mit `git log`
vergleichen, ob der Quellcode seither weiter geändert wurde.

## Inhalt

```
firmware/
├── ESPMaster/
│   ├── ESPMaster.bin   - Sketch-Binary für den ESP8266 (Master-Modul)
│   └── littlefs.bin    - Dateisystem-Image (Web-UI: index.html, admin.html, ...)
├── Unit/
│   ├── Unit.hex         - App-Firmware für die Units (Intel HEX, für ISP/Arduino-IDE-Upload)
│   └── Unit.bin         - Dieselbe Firmware als rohes Binary (für den I2C-Firmware-Update-Push
│                          über die ESPMaster Admin-Seite, siehe Unit/BOOTLOADER_SETUP.md)
└── bootloader/
    ├── twiboot.hex       - Bootloader-Firmware (Intel HEX, für ISP-Flash)
    └── twiboot.bin       - Dasselbe als rohes Binary
```

## Build-Konfiguration (wie diese Dateien entstanden sind)

**ESPMaster** (`esp8266:esp8266:generic`, Flash-Layout `eesz=4M1M`):
```bash
arduino-cli compile --fqbn "esp8266:esp8266:generic:eesz=4M1M" ESPMaster
```
**Änderung 2026-08-21:** vorher `eesz=1M128`
(~438KB OTA-Slot). Per neuem `/api/heap`-Diagnosefeld
(`ESP.getFlashChipRealSize()`) wurde festgestellt, dass der tatsächlich
verbaute Chip 4MB groß ist - vorher nur zu einem Viertel genutzt, und der
OTA-Slot war bereits kleiner als der Sketch selbst (Web-Firmware-Upload
schlug deshalb strukturell immer fehl). Auf `eesz=4M1M` umgestellt
(~1019KB OTA-Slot, 1MB LittleFS). **Wichtig, falls dieser Chip je gegen
ein anderes Modul getauscht wird:** vor einem Rebuild mit diesem Profil
zwingend erst `flashReal` gegen `flashConfigured` in `/api/heap` prüfen -
ein zu groß gewähltes `eesz` für einen kleineren echten Chip kann beim
Boot zu Adressfehlern führen.

`littlefs.bin` gebaut mit `mklittlefs -p 256 -b 8192 -s 1024000` (Werte
passend zu `eesz=4M1M`s `spiffs_blocksize`/`spiffs_start`/`spiffs_end` in
`boards.txt` - **nicht** mehr die alten `4096`/`131072`-Werte). Adresse zum
Flashen ebenfalls geändert: `0x300000` statt `0xDB000` (siehe `boards.txt`s
`generic.menu.eesz.4M1M.build.spiffs_start`).

**Änderung 2026-08-31:** `mklittlefs` läuft nicht
mehr direkt gegen [`ESPMaster/data/`](../ESPMaster/data), sondern gegen eine
Kopie davon, in der `index.html` durch ein gzip-komprimiertes
`index.html.gz` ersetzt ist (`gzip -9 -k -c ESPMaster/data/index.html >
<kopie>/index.html.gz`, danach das unkomprimierte `index.html` aus der Kopie
löschen) - spart ~55% Übertragungsgröße, `ESPAsyncWebSrv` liefert `.gz`-Dateien
automatisch mit korrektem `Content-Encoding: gzip` aus. Die im Repo verfolgte
`ESPMaster/data/index.html` selbst bleibt unverändert die normale, lesbare
Quelldatei - nur dieser Build-Schritt komprimiert sie zusätzlich. Einzelne
Web-Datei-Updates über `POST /api/web-file` müssen seitdem ebenfalls
`index.html.gz` (nicht mehr `index.html`) als Zieldateiname verwenden.

**Unit** (Arduino Nano / bare ATmega328P, altes Bootloader-Profil - passend
zu den 16MHz/`hfuse=0xDA`-Fuses, siehe [`Unit/BOOTLOADER_SETUP.md`](../Unit/BOOTLOADER_SETUP.md)):
```bash
arduino-cli compile --fqbn arduino:avr:nano:cpu=atmega328old Unit
```

**Bootloader** (gepatchter twiboot, siehe [`Unit/bootloader/twiboot/main.c`](../Unit/bootloader/twiboot/main.c)
für Details/Attribution):
```bash
cd Unit/bootloader/twiboot
make MCU=atmega328p CFLAGS_TARGET="-DUSE_CLOCKSTRETCH=1"
```

## Flashen

- **ESPMaster:** `ESPMaster.bin` per `arduino-cli upload`/`esptool.py` auf
  Adresse `0x0`, danach `littlefs.bin` auf Adresse `0x300000` (seit
  2026-08-21/Punkt 83 - vorher `0xDB000`, siehe Haupt-[README](../README.md),
  Abschnitt "Uploading the Static Assets", dort noch die alte Adresse
  dokumentiert). **Seit 2026-08-22/Punkt 88 zuverlässig auch per Browser
  möglich** (nur den Sketch, kein LittleFS) - über die Admin-Seite, die den
  Sketch in 4KB-Häppchen über `/api/firmware-start`/`-chunk`/`-finish`
  hochlädt. Der serielle Programmer bleibt nützlich für Diagnose
  (Serial-Log), ist für normale Updates aber nicht mehr zwingend nötig.
- **Unit-App:** `Unit.hex` per ISP oder Arduino-IDE - oder, falls die Unit
  bereits den twiboot-Bootloader hat, `Unit.bin` über die ESPMaster
  Admin-Seite ("Unit Firmware Update") per I2C pushen, kein USB nötig.
- **Bootloader:** `twiboot.hex` per ISP-Programmer, siehe
  [`Unit/BOOTLOADER_SETUP.md`](../Unit/BOOTLOADER_SETUP.md) für die
  vollständige, verifizierte Anleitung inkl. Fuse-Werten - **einmalig pro
  Unit, physischer Eingriff an bereits verbauter Hardware, dort zuerst den
  Testplan durchgehen.**
