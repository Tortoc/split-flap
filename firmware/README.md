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

**ESPMaster** (`esp8266:esp8266:generic`, Flash-Layout `eesz=1M128`):
```bash
arduino-cli compile --fqbn "esp8266:esp8266:generic:eesz=1M128" ESPMaster
```
`littlefs.bin` gebaut mit `mklittlefs -p 256 -b 4096 -s 131072` aus
[`ESPMaster/data/`](../ESPMaster/data).

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
  Adresse `0x0`, danach `littlefs.bin` auf Adresse `0xDB000` (siehe
  Haupt-[README](../README.md), Abschnitt "Uploading the Static Assets").
- **Unit-App:** `Unit.hex` per ISP oder Arduino-IDE - oder, falls die Unit
  bereits den twiboot-Bootloader hat, `Unit.bin` über die ESPMaster
  Admin-Seite ("Unit Firmware Update") per I2C pushen, kein USB nötig.
- **Bootloader:** `twiboot.hex` per ISP-Programmer, siehe
  [`Unit/BOOTLOADER_SETUP.md`](../Unit/BOOTLOADER_SETUP.md) für die
  vollständige, verifizierte Anleitung inkl. Fuse-Werten - **einmalig pro
  Unit, physischer Eingriff an bereits verbauter Hardware, dort zuerst den
  Testplan durchgehen.**
