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
│   └── littlefs.bin    - Dateisystem-Image (Web-UI: index.html.gz)
├── Unit/
│   ├── Unit.hex         - App-Firmware für die Units (Intel HEX, für ISP/Arduino-IDE-Upload)
│   └── Unit.bin         - Dieselbe Firmware als rohes Binary (für den I2C-Firmware-Update-Push
│                          über die ESPMaster-Weboberfläche, siehe Unit/BOOTLOADER_SETUP.md)
└── bootloader/
    ├── twiboot.hex       - Bootloader-Firmware (Intel HEX, für ISP-Flash)
    └── twiboot.bin       - Dasselbe als rohes Binary
```

## Build-Konfiguration (wie diese Dateien entstanden sind)

⚠️ **Immer `./compile-espmaster.sh` benutzen, nicht `arduino-cli compile` von Hand tippen.**
Dieses Skript (in diesem Ordner) verdrahtet `:eesz=4M1M,mmu=4816H` und die `-Wl,--wrap=...`-Flags fest -
beide wurden in der Vergangenheit schon mehrfach vergessen (zuletzt 2026-09-16), mit stundenlanger Fehlsuche in
die falsche Richtung (WLAN-Flakiness) als Folge. Die Befehle unten sind nur zur Dokumentation,
was das Skript intern tut - nicht zum Abtippen.

⚠️ **Das `:eesz=4M1M` ist Teil des FQBN, kein optionaler Zusatz.** Ein Build
mit dem bloßen `esp8266:esp8266:generic` (ohne `eesz`) kompiliert still und
ohne Fehler - aber mit einem falschen (kleineren) Flash-Layout, das die
echte LittleFS-Partition und die WLAN-Zugangsdaten unsichtbar macht (Gerät
verhält sich wie zurückgesetzt: eigener AP-Modus, leere Einstellungen) und
gleichzeitig den OTA-Slot auf ~438KB schrumpft, was auch die
Web-OTA-Selbstreparatur mit dem korrekten Build vereitelt (Absturz mitten im
Chunk-Upload). **Nach JEDEM Compile+Flash zwingend `flashConfigured` gegen
`flashReal` in `GET /api/diag?type=heap` prüfen**, nicht erst bei einem
Chip-Tausch (2026-09-08/09 hier schonmal aufgetreten).
Falls es doch passiert: Web-OTA kann sich selbst nicht reparieren (siehe
oben) - Recovery nur per seriellem `arduino-cli upload`/`esptool.py`, das
unabhängig vom aktuell laufenden (falschen) Partitionsschema an absoluten
Flash-Adressen schreibt.

**ESPMaster** (`esp8266:esp8266:generic`, Flash-Layout `eesz=4M1M`, MMU `4816H`):
```bash
arduino-cli compile --fqbn "esp8266:esp8266:generic:eesz=4M1M,mmu=4816H" \
  --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=_Znwj -Wl,--wrap=tcp_accept" \
  ESPMaster
```
**⚠️ Das `--build-property` mit den beiden `-Wl,--wrap=...`-Flags ist seit Punkt 207/209
(2026-09-12) ZWINGEND erforderlich, kein optionaler Zusatz.** `ServiceMemoryReserveFunctions.cpp`
und `ServiceConnectionGateFunctions.cpp` definieren `__wrap__Znwj`/`__wrap_tcp_accept` und
referenzieren `__real__Znwj`/`__real_tcp_accept` - letztere existieren als linkbare Symbole nur,
wenn der Linker tatsächlich mit `--wrap=_Znwj`/`--wrap=tcp_accept` aufgerufen wird. Ohne dieses
Flag schlägt der Build mit "undefined reference to `__real__Znwj`" (bzw. `__real_tcp_accept`) fehl
- das ist beabsichtigt (ein stiller Fallback auf ungeschütztes Verhalten wäre schlimmer als ein
lauter Build-Fehler). Der Mechanismus selbst ist nicht neu erfunden: dieser Core nutzt exakt
dasselbe `-Wl,-wrap,...`-Muster bereits selbst für zwei andere Symbole
(`compiler.c.elf.flags` in `platform.txt`: `-Wl,-wrap,system_restart_local
-Wl,-wrap,spi_flash_read`).

**`mmu=4816H` (seit ESPMaster 4.10.11):** 16 KB Flash-Cache statt 32 KB, dafür 48 KB IRAM; was
der Code davon nicht belegt (~19 KB), wird ein zweiter Heap. Dort landet nur, was gezielt unter
`HeapSelectIram` allokiert wird - derzeit die beiden 4-KB-OTA-Puffer, damit ein Upload bei
offener Weboberfläche nicht am knappen DRAM scheitert (Code-Review V7, Punkt C). Fehlt die
Option, baut der Sketch trotzdem; die Puffer liegen dann wie früher im DRAM. Freier IRAM-Heap:
`iramFree` in `GET /api/diag?type=heap`.

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
  2026-08-21/Punkt 83 - vorher `0xDB000`). **Seit 2026-08-22/Punkt 88
  zuverlässig auch per Browser möglich** (nur den Sketch, kein LittleFS) - über
  ⋮ Menü > Firmware Update > ESPMaster, das den Sketch in 4KB-Häppchen über
  `/api/firmware-start`/`-chunk`/`-finish` hochlädt. Die Weboberfläche allein
  lässt sich per `POST /api/web-file` (`index.html.gz`) ersetzen. Der serielle
  Programmer bleibt nützlich für Diagnose (Serial-Log), ist für normale Updates
  aber nicht mehr zwingend nötig.
- **ESPMaster 5.0.0:** Der erste Start wandelt das Admin-Passwort in einen Hash um.
  Ein Primärgerät vor 5.0.0 kann ein geschütztes Erweiterungsgerät mit 5.0.0 nicht
  mehr ansteuern - im Verbund beide Geräte updaten und danach koppeln (siehe
  Haupt-[README](../README.md#multiple-displays)).
- **Unit-App:** `Unit.hex` per ISP oder Arduino-IDE - oder, falls die Unit
  bereits den twiboot-Bootloader hat, `Unit.bin` über die ESPMaster-Weboberfläche
  (⋮ Menü > Firmware Update > Units) per I2C pushen, kein USB nötig.
- **Bootloader:** `twiboot.hex` per ISP-Programmer, siehe
  [`Unit/BOOTLOADER_SETUP.md`](../Unit/BOOTLOADER_SETUP.md) für die
  vollständige, verifizierte Anleitung inkl. Fuse-Werten - **einmalig pro
  Unit, physischer Eingriff an bereits verbauter Hardware, dort zuerst den
  Testplan durchgehen.**
