# Bootloader-Setup für I2C-Firmware-Updates (twiboot, gepatcht)

Dieses Dokument beschreibt den **einmaligen, physischen** Vorgang, um eine
Unit (bare ATmega328P auf der [fae-pcb](https://github.com/codingcatgirl/split-flap-fae-pcb))
für Firmware-Updates über I2C vorzubereiten. Danach lassen sich künftige
`Unit.ino`-Updates für diese Unit über die ESPMaster-Weboberfläche (⋮ Menü >
Firmware Update > Units) ausrollen,
ohne sie nochmal per USB/ISP anzufassen.

> ⚠️ **Das hier ist ein Eingriff an echter, bereits verbauter Hardware.**
> Zuerst vollständig an einer Ersatz-/Basteltisch-Unit durchführen und
> testen (siehe Testplan weiter unten), bevor irgendeine bereits verbaute
> Unit angefasst wird. Ein fehlerhaft gesetzter Fuse-Wert kann eine Unit
> unbrauchbar machen, bis sie mit einem "High-Voltage Programmer" gerettet
> wird - siehe auch den Abschnitt "Aus Schaden klug geworden" unten, das ist
> uns beim Entwickeln dieses Patches selbst passiert.

## Wichtig: Diese Hardware ist KEIN Arduino Nano

Das Haupt-[README](../README.md) beschreibt einen Arduino Nano auf einer
Trägerplatine. **Die tatsächlich hier verwendete Hardware ist die
[fae-pcb](https://github.com/codingcatgirl/split-flap-fae-pcb)**: ein bare
ATmega328P direkt auf der Platine, 3.3V-Logik (kein 5V-Nano-Modul), mit
eigenem 16MHz-Quarz (Y2 im Schaltplan). Der Firmware-Code in diesem Repo ist
bereits an diese Hardware angepasst (Fork von
[evilscientress/split-flap](https://github.com/evilscientress/split-flap)),
aber **Fuses/Taktkonfiguration für den Bootloader müssen zu genau dieser
Platine passen**, nicht zu einem generischen Nano oder zu twiboots eigenen
Beispielwerten - siehe unten.

## Warum "gepatcht"?

Die I2C-Adresse einer Unit wird hardwareseitig per DIP-Schalter/Jumper
gesetzt (siehe "Set Unit Address" im Haupt-README) - das soll für
Firmware-Updates genauso funktionieren wie für den normalen Betrieb. Der
unveränderte [twiboot](https://github.com/orempel/twiboot) liest seine
I2C-Adresse aber nur aus einem fest einkompilierten Wert, kennt die
DIP-Schalter nicht.

`Unit/bootloader/twiboot/main.c` in diesem Repo ist deshalb eine **leicht
gepatchte Kopie** (nicht mehr das unveränderte Upstream-Projekt, siehe
Kommentarblock am Dateianfang für Details/Attribution): Der Bootloader liest
beim Start dieselben DIP-Pins wie `Unit.ino`s `getaddress()` und berechnet
seine I2C-Adresse daraus. **Seit 2026-08-23 sind das 6 Pins** (A0-A3 + D3/D4,
Adressen 0-63 statt vorher 4 Pins/0-15) - siehe den Kommentar auf
`addressPins` in `Unit.ino` für die genaue Pin-Zuordnung.
Ergebnis:

- **Ein einziges** kompiliertes Bootloader-Image für **alle** Units - keine
  Adress-Buchhaltungstabelle mehr nötig.
- DIP-Schalter/Jumper umstecken wirkt **sofort** auf Bootloader und App
  gleichermaßen, ganz ohne erneuten Bootloader-Flash.

## Was wird benötigt

- Ein ISP-Programmer (z.B. USBasp, AVRISP mkII, oder ein zweiter
  Arduino als ["Arduino as ISP"](https://docs.arduino.cc/built-in-examples/arduino-isp/ArduinoISP/))
- `avr-gcc`/`make`/`avrdude` (im Arduino-IDE-Toolchain bereits enthalten,
  oder separat installierbar, z.B. via `avr-gcc`/`avr-libc`/`avrdude`-Pakete
  der jeweiligen Linux-Distribution)
- Die Dateien unter `Unit/bootloader/twiboot/` in diesem Repo (kein
  Submodule/Checkout nötig, liegen bereits im Repo)

## ⚠️ Aus Schaden klug geworden: Taktkonfiguration ist hardware-spezifisch

Der allererste Patch-Entwurf hat twiboots eigene Beispiel-Fuses übernommen
(`lfuse=0xC2`, interner 8MHz-RC-Oszillator) - **das war falsch für diese
Hardware** und hat den Schrittmotor komplett lahmgelegt (Timing lief mit
falscher Taktrate). Die anschließende Korrektur auf einen generischen
"Arduino Nano"-Wert traf danach die falsche Bootloader-Größe. Erst das
**Auslesen der Fuses einer unberührten, original programmierten Ersatz-Unit**
hat die tatsächlich korrekten Werte geliefert - nicht raten, sondern messen:

```bash
avrdude -c usbasp -p m328p -B 125kHz -U lfuse:r:-:h -U hfuse:r:-:h -U efuse:r:-:h
```

**Verifiziert (2026-08-10, an echter Hardware getestet, nicht nur berechnet):**

| Fuse | Wert | Bedeutung |
| --- | --- | --- |
| `lfuse` | `0xFF` | Externer Quarz (16MHz, Y2), NICHT twiboots eigener Beispielwert `0xC2` (interner 8MHz-RC) |
| `hfuse` | `0xDA` | 1024 Words Bootloader-Bereich, Start bei `0x7800` - NICHT twiboots `0xDC`/512 Words/`0x7C00` |
| `efuse` | `0xFD` | 2.7V BOD (unverändert, stimmte schon in twiboots Beispiel) |

Passend dazu in `main.c`: `F_CPU=16000000ULL` (nicht twiboots 8MHz) und
`TIMER_IRQFREQ_MS=12` (nicht twiboots 25 - bei 16MHz würde der 8-Bit-Timer0
sonst überlaufen, siehe Kommentare im Code). Alle vier Werte (`lfuse`,
`hfuse`/`BOOTLOADER_START`, `F_CPU`, `TIMER_IRQFREQ_MS`) müssen zueinander
UND zur echten Hardware passen - **ein falscher Wert erzeugt keine
Fehlermeldung, sondern lässt den Chip einfach mit falscher Taktrate oder an
der falschen Bootloader-Adresse laufen.** Diese Werte sind bereits als
Standard im `Makefile` und in `main.c` hinterlegt - solltet ihr eine andere
Hardware-Revision verwenden, unbedingt zuerst gegen eine unberührte Unit
verifizieren wie oben, nicht diese Werte blind übernehmen.

## Schritt 1: Bauen und Flash-Budget prüfen (zwingend vor jedem ISP-Flash)

```bash
cd Unit/bootloader/twiboot
make clean
make MCU=atmega328p CFLAGS_TARGET="-DUSE_CLOCKSTRETCH=1"
```

`-DUSE_CLOCKSTRETCH=1` lässt den Bootloader während des eigentlichen
Flash-Schreibens die I2C-Clock-Leitung sauber halten (Clock-Stretching), was
ESPMaster als Master nativ unterstützt (`Wire.setClockStretchLimit()`) -
ohne dieses Flag müsste ESPMaster den genauen Timing-Ablauf raten.

**Danach zwingend die `avr-size`-Ausgabe prüfen**, die `make` automatisch
mit ausgibt (`text` + `data` = tatsächlicher Flash-Verbrauch, `bss` ist nur
RAM und zählt nicht). Der Bootloader-Bereich ist durch die BOOTSZ-Fuse hart
auf **1024 Words = 2048 Bytes** begrenzt (siehe oben, `hfuse=0xDA`).

**Verifiziert (2026-08-10, `avr-gcc` 16.1.0 / `avr-libc` 2.3.2):** Flash-
Verbrauch (text+data) **746 Bytes von 2048 Bytes** - komfortabel im Budget.

**Nachtrag (2026-09-09):** EEPROM-Update-Marker ergänzt (siehe Change 2 im
Datei-Kopfkommentar von `main.c` - verhindert, dass ein abgebrochenes Update
nach einem erneuten Reset in inkonsistenten Alt-/Neu-Code-Mix bootet).
Neu verifiziert: **790 Bytes von 2048 Bytes** (+44 Bytes) - weiterhin
komfortabel im Budget (39%).

**Zweiter Nachtrag (2026-09-09):** Idle-Boot-Timeout repariert (siehe Change
3 im Datei-Kopfkommentar von `main.c`, CODE_REVIEW.md #9 - `boot_timeout`
wurde vorher von jedem echten Bootloader-Kommando dauerhaft auf 0 gesetzt
und nie wieder aktiviert, statt einen rollierenden Idle-Timer zu bilden;
eine abgebrochene Sitzung VOR dem Scharfstellen des EEPROM-Markers blieb
dadurch ohne funktionierenden automatischen Rückweg im Bootloader stecken -
live reproduziert, brauchte einen manuellen Stromzyklus). Neu verifiziert:
**792 Bytes von 2048 Bytes** (+2 Bytes) - weiterhin komfortabel im Budget
(39%).

## Schritt 2: ISP-Flash - identisch für jede Unit

**Nur mit der gerade angeschlossenen Unit**, ISP-Programmer entsprechend
anpassen (Beispiel für einen USBasp-Klon):

```bash
# Bootloader schreiben (löscht dabei den GESAMTEN Chip inkl. der App!)
make MCU=atmega328p install AVRDUDE_PROG="-c usbasp -B 125kHz"

# Fuses setzen (separat! passiert NICHT automatisch mit "install")
make MCU=atmega328p fuses AVRDUDE_PROG="-c usbasp -B 125kHz"
```

`-B 125kHz` senkt die ISP-Taktrate; ohne das kam es bei uns zu
"target does not answer"-Fehlern. `MCU=atmega328p` muss bei **jedem**
`make`-Aufruf explizit angegeben werden (auch bei `install`/`fuses`),
sonst fällt das Makefile auf den ATtiny85-Standard zurück.

Für "Arduino as ISP" typischerweise stattdessen z.B.
`AVRDUDE_PROG="-c stk500v1 -b 19200 -P /dev/ttyUSB0"` (Port anpassen).

Dasselbe `twiboot.hex` wird auf **jede** Unit geflasht - unabhängig davon,
auf welche Adresse ihre DIP-Schalter aktuell stehen.

**Wichtig:** Der `install`-Schritt löscht per ISP-Chip-Erase den gesamten
Flash-Speicher, also auch eine eventuell schon vorhandene `Unit.ino`-App!
Direkt danach die App erneut flashen (z.B. über die Arduino IDE per ISP,
oder `avrdude ... -D -U flash:w:Unit.ino.hex:i` mit `-D`, um den gerade
geschriebenen Bootloader dabei nicht wieder zu löschen).

**Gilt genauso in die andere Richtung, nicht nur beim allerersten Setup**
(live erlebt, 2026-09-09): Wird später,
NACHDEM Bootloader+App beide schon korrekt drauf sind, per ISP nur die App
NOCHMAL neu geschrieben (z.B. um einen EEPROM-Wert/eine Fehlkalibrierung zu
beheben), löscht `avrdude` ohne `-D` **wieder den kompletten Chip inklusive
des bereits vorhandenen Bootloaders** - der Schreibvorgang selbst
(nur die App betreffend) läuft dabei erfolgreich und verifiziert sauber,
der Bootloader-Verlust fällt also nicht sofort auf. Ergebnis: die Unit
läuft scheinbar normal weiter (bootet direkt in die App), aber
I2C-Firmware-Updates schlagen ab sofort mit "Unit did not respond in
bootloader mode" fehl, weil schlicht kein Bootloader mehr da ist.
**Faustregel: Jeder ISP-Schreibvorgang, der NICHT explizit alle drei
Bereiche (Bootloader + App + Fuses) neu setzen soll, braucht `-D`.** Nach
jedem ISP-Zugriff, der nur einen Teil ändern sollte, den unberührt
bleibenden Teil per `avrdude -c ... -p atmega328p -t` und
`dump flash 0x7800 32` (Bootloader) bzw. `dump flash 0x0 16` (App-Vektortabelle)
gegenprüfen - nicht nur den gerade geschriebenen Teil.

**Einschränkung dieser Faustregel** (live erlebt, 2026-09-09): `-D` funktioniert nur, wenn der Zielbereich schon
**blank (`0xFF`)** ist - AVR-Flash kann ohne vorheriges Löschen nur Bits von
1→0 setzen, nie zurück. Beim Versuch, per `-D` NUR den Bootloader auf einer
Unit zu aktualisieren, die schon einen (alten) Bootloader hatte, entstand
dadurch ein inkonsistenter Bit-Mix aus altem und neuem Inhalt (avrdude
meldete einen Verifikationsfehler, `Warning: flash verification mismatch`) -
sofort per vollem Chip-Erase + Neuschreiben beider Bereiche korrigiert.
**Praktische Konsequenz:** ein bereits programmierter Bootloader kann NICHT
in-place per ISP aktualisiert werden, ohne die App mit zu löschen - dafür
gibt es keinen Weg, der beide Bereiche gleichzeitig erhält. Die volle Sequenz
lautet dann: `avrdude -U flash:w:twiboot.hex:i` (ohne `-D`, kompletter
Chip-Erase, App geht verloren) → App per I2C-Push wiederherstellen (siehe
`ServiceUnitFirmwareFunctions.ino`s `pushFirmwareToUnit()` - genau der jetzt
durch Punkt 137 abgesicherte Weg) → Kalibrierungs-Offset erneut setzen (vorher
per EEPROM-Dump sichern, siehe Beispiel: `dump eeprom 0 4` zeigt `calOffset`
als Little-Endian-`uint16_t` in den ersten beiden Bytes).

## Schritt 3: Smoke-Test (an der Ersatz-Unit, vor jeder verbauten Unit)

Direkt nach `make install` + `make fuses` + erneutem App-Flash, **bevor**
die nächste Unit angefasst wird:

1. Normale `Unit.ino`-App läuft weiterhin normal (I2C-Adresse, Homing,
   Buchstaben-Anzeige) - der Bootloader mischt sich nicht ein, solange kein
   Update ausgelöst wird. **Mehrfach per Reset/Stromzyklus testen**, nicht
   nur einmal - wacklige ISP-Verkabelung kann einzelne Tests verfälschen.
2. **Jumper-Test (das eigentliche Ziel dieses Patches):** DIP-Schalter der
   Unit auf einen bestimmten Wert stellen, Unit per Reset in den Bootloader
   schicken (z.B. über einen Update-Versuch in der Admin-UI), per
   `/admin/units`-Scan bestätigen, dass twiboot exakt an
   `0x29 + <eingestellter DIP-Wert>` antwortet. Danach DIP-Schalter auf
   einen **anderen** Wert umstellen, Unit per Stromzyklus resetten, erneut
   prüfen: Bootloader antwortet jetzt an der **neuen** Adresse - ganz ohne
   erneuten ISP-Flash. Erst wenn das zuverlässig klappt, weiter zu Punkt 3.
3. Über die ESPMaster-Weboberfläche (⋮ Menü > Firmware Update > Units) einen
   vollständigen Push einer (funktional unveränderten) neu kompilierten
   `Unit.ino` an genau diese Unit durchführen, danach nochmal normalen
   Betrieb prüfen.
4. Ein absichtlich abgebrochenes Update simulieren (z.B. ESPMaster mitten
   im Push neu starten) und prüfen, dass ein zweiter Versuch die Unit
   wieder normal zum Laufen bringt.

Erst wenn alle vier Punkte an der Ersatz-Unit sauber funktionieren, mit den
tatsächlich verbauten Units fortfahren - eine nach der anderen, mit
demselben Smoke-Test nach jeder einzelnen.

## Hintergrund: Warum der Master trotzdem noch scannt

ESPMaster findet die Bootloader-Adresse einer Unit beim Update-Push per
I2C-Scan (`0x29`-`0x68`, seit der 6-Bit-Erweiterung 2026-08-23 - vorher
`0x29`-`0x38` für den alten 4-Bit-Bereich), statt
sie stur aus dem DIP-Wert zu berechnen. Da Bootloader und App jetzt immer
dieselben Jumper lesen, sind beide Adressen praktisch immer synchron - der
Scan ist also keine Notwendigkeit mehr, bleibt aber als zusätzliche
Absicherung bestehen (z.B. falls durch einen gemeinsamen Stromausfall
kurzzeitig mehr als eine Unit gleichzeitig im Bootloader-Fenster steckt -
der Scan bricht dann sauber mit einer klaren Fehlermeldung ab, statt zu
raten). Der normale App-seitige Boot-Scan in ESPMaster (`scanI2CBus()`,
zur Erkennung von `unitsAmount`) wurde beim selben Fix passend auf
`0`-`63` erweitert.
