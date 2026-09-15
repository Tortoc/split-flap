# Mehrgeräte-Verbund (Primärgerät + Erweiterungsgerät) - Architekturplan

**Status (2026-09-15): Phase 1 implementiert, kompiliert sauber, NOCH NICHT auf echter Hardware
verifiziert** - beide Testgeräte (192.168.1.51/.195) waren zum Zeitpunkt der Umsetzung nicht
erreichbar. Neue Dateien: `ServiceExtensionFunctions.ino` (Health-Check, Push, Empfang),
`Classes.h`s `ExtensionDevice`-Struct. Geänderte Dateien: `ESPMaster.ino` (Rollen-Einstellung,
Erweiterungsgeräte-Liste, neue Endpunkte `/api/device-role`, `/api/extension-devices`,
`/api/extension-display`, `deviceRole` im Boot-Snapshot), `HelpersStringHandling.ino`
(`centerString()`/`leftString()`/`rightString()`/`processSentenceToLines()` bekommen einen
`width`-Parameter statt der globalen `unitsAmount`), `ServiceDisplayJobFunctions.ino` (nutzt
`combinedUnitsAmount()` an den 5 relevanten Stellen, ruft `pushExtensionSegments()` einmal pro
neuer Zeile), `data/index.html` (neuer "Mehrgeräte-Verbund"-Tab). Sobald mindestens ein Gerät
läuft: Extension-Empfang isoliert testbar (Rolle setzen, `POST /api/extension-display` direkt per
curl simulieren); für den vollen Primärgerät-Pfad werden zwei laufende Geräte gebraucht.

## Kontext

Nutzerwunsch: zwei (oder mehr) physisch getrennte Split-Flap-Geräte (je eigenes ESPMaster + eigene
Units) über WLAN so verbinden, dass eines die komplette Anzeige-Logik trägt und die anderen
lediglich seine Anzeigefläche verlängern - das erste Gerät soll wissen, wie viele Units das/die
anderen Geräte haben, und den Text entsprechend über die kombinierte Breite umbrechen/ausrichten.

**Begriffe:** "Master/Slave" wurde auf Wunsch des Nutzers durch ethisch unbedenkliche Begriffe
ersetzt: **Primärgerät** (kurz: Primary, `deviceRole=primary`) für das Gerät mit der
Anzeige-Logik, **Erweiterungsgerät** (kurz: Extension, `deviceRole=extension`) für jedes Gerät,
das nur zusätzliche Anzeigefläche beisteuert. Neuer, dritter Wert `deviceRole=standalone` ist der
Default und entspricht exakt dem heutigen, unveränderten Verhalten - der ganze Verbund ist damit
vollständig opt-in und rückwärtskompatibel.

## Warum das ein größeres Vorhaben ist

`unitsAmount` ist heute an über 50 Stellen fest verankert - Zeilenumbruch
(`processSentenceToLines()`), Ausrichtung (`centerString()`/`leftString()`/`rightString()`),
Animations-Timing (`computeUnitAnimationDelayMs()`), der komplette I2C-Schreib-/Wartezyklus
(`ServiceDisplayJobFunctions.ino`s `DisplayJob`-Zustandsmaschine, seit Punkt 81 bewusst
nicht-blockierend gebaut). Ein Verbund heißt im Kern: die effektive Breite wird zu
`eigeneUnits + Summe(Extension-Units)`, aber nur das Primärgerät soll die "Intelligenz" (Text-
Logik, Rotation, Countdown etc.) besitzen - jedes Erweiterungsgerät bleibt für diesen Zweck ein
"dummer" Anzeige-Abschnitt, dem exakt gesagt wird, welche Zeichen auf seinen eigenen Units stehen
sollen.

## Architektur

### 1. Rollen-Konfiguration (neue, persistierte Einstellung)

`deviceRole = standalone | primary | extension` (Default `standalone`). Auf einem Primärgerät
zusätzlich eine konfigurierbare Liste bekannter Erweiterungsgeräte (Hostname/IP + optionaler
Anzeigename je Eintrag) - **manuell im Web-UI eingetragen**, keine automatische mDNS-Suche: die
bestehende mDNS-Unterstützung (Punkt 183) kann einen Namen nur *ankündigen*, aktives *Durchsuchen*
ist auf diesem Core deutlich unfertiger/fragiler - bei der hier realistischen Größenordnung
(1-2 Geräte im selben Haushalt) ist eine einmalige manuelle Kopplung robuster und einfacher zu
debuggen als ein automatischer Such-Mechanismus.

Ein Erweiterungsgerät selbst braucht **keine** Kenntnis seines Primärgeräts - das Primärgerät
initiiert jeden Kontakt (Statusabfrage wie Anzeige-Push). Das hält die Kopplung einseitig und
einfach: ein Erweiterungsgerät weiß nur "ich bin gerade im Erweiterungsgerät-Modus", nicht "wer
mein Primärgerät ist".

### 2. Unit-Zählung/Erreichbarkeit (Wiederverwendung, kein neuer Endpunkt nötig)

Das Primärgerät pollt jedes konfigurierte Erweiterungsgerät periodisch über das **bereits
bestehende** `GET /api/units` (liefert schon `unitsAmount`) - kein neuer Endpunkt für diesen Teil
nötig. Letzter erfolgreicher Kontakt wird pro Erweiterungsgerät gespeichert (gleiches
"Health-Check"-Muster wie das bestehende periodische Stabilitäts-Monitoring dieses Projekts).
Bleibt ein Erweiterungsgerät zu lange ohne Antwort: als nicht erreichbar markiert, **aus der
kombinierten Breite herausgerechnet** (Fail-Soft) - das Primärgerät zeigt weiterhin seinen eigenen
Anteil normal an, statt den gesamten Verbund einfrieren zu lassen. Dieses Verhalten ist bewusst
gewählt, aber eine spätere Session sollte es explizit mit dem Nutzer bestätigen (Alternative wäre
z. B. eine Fehleranzeige auf dem Primärgerät selbst).

### 3. Anzeige-Protokoll: neuer Push-Endpunkt auf dem Erweiterungsgerät

Neuer, passwortgeschützter Endpunkt (gleiches `X-OTA-Password`-Muster wie `/api/text`/
`/api/units/firmware-push`) - Arbeitstitel `POST /api/extension-display`. Body: die fertig
vorbereiteten Zeichen für GENAU die eigenen Units dieses Geräts (0..eigeneUnits-1), vom
Primärgerät bereits aus der Gesamtzeile herausgeschnitten. Das Erweiterungsgerät braucht dafür
**keine eigene** Zeilenumbruch-/Ausrichtungslogik - es schreibt die empfangenen Zeichen direkt
über seinen bestehenden, bereits vorhandenen Low-Level-I2C-Schreibpfad auf seine eigenen Units
(Wiederverwendung, kein Parallel-Code).

**Kein SSL/TLS** - reines HTTP im lokalen WLAN, wie jeder andere interne Endpunkt dieses Projekts.
Auf Nutzerfrage (2026-09-14) verifiziert: **IRAM-neutral.** Zwei isolierte Test-Sketches (nur
`WiFiClient` mit von Hand gebautem Request vs. komplette `HTTPClient`-Bibliothek) kompilierten auf
Byte identisches IRAM (26.375 Byte) - der gesamte IRAM-Fußabdruck kommt vollständig vom
WiFi/SDK-Unterbau, den dieses Projekt bereits zu 100% bezahlt; die HTTP-Client-Schicht selbst
(roh oder komfortabel) trägt nichts weiter bei. `WiFiClientSecure`/BearSSL (für HTTPS) wäre der
Punkt, an dem IRAM tatsächlich eng werden könnte (bekannt rechenintensive Kryptoroutinen) - aber
genau das ist hier bewusst nicht vorgesehen.

**Wichtig:** Ein Erweiterungsgerät behält seine eigene, vollständig nutzbare Web-Oberfläche/API
(Kalibrierung, Unit-Verwaltung, Firmware-Updates, WLAN-Einstellungen) - nur die "was wird gerade
angezeigt"-Entscheidung wird im Erweiterungsgerät-Modus an das Primärgerät abgetreten
(`performOneSecondDisplayTick()`s normaler Modus-Dispatch pausiert, solange `deviceRole=extension`
UND ein Primärgerät gerade aktiv Inhalte pusht).

### 4. Primärgerät: kombinierte Breite in der bestehenden Pipeline

`processSentenceToLines()`/`centerString()`/`leftString()`/`rightString()` bekommen die
Gesamtbreite (`eigeneUnits + Summe(erreichbare Extension-Units)`) statt implizit das globale
`unitsAmount` zu lesen - kleinster gemeinsamer Nenner-Umbau: diese Funktionen brauchen einen
Breiten-**Parameter** statt einer globalen Variable. Nach der Zeilenberechnung schneidet das
Primärgerät die fertige Zeile in Abschnitte: die ersten `eigeneUnits` Zeichen gehen wie bisher über
den lokalen I2C-Pfad, jeder weitere Abschnitt wird per `POST /api/extension-display` an das
jeweilige Erweiterungsgerät geschickt (Reihenfolge = Konfigurationsreihenfolge der
Erweiterungsgeräte-Liste).

### 5. Animations-Koordination über Geräte hinweg (bewusst NICHT Phase 1)

`computeUnitAnimationDelayMs()` müsste einen globalen statt lokalen Unit-Index bekommen, damit z. B.
"außen nach innen" über den ganzen Verbund hinweg konsistent aussieht. Deutlich mehr Entwurfsarbeit
(Zeitsynchronisation zwischen Geräten, Netzwerklatenz) - explizit auf Phase 2 verschoben, siehe unten.

## Phasenplan (empfohlen)

- **Phase 1 (kleinster sinnvoller Umfang):** nur `deviceMode=text` (kein Rotation/Countdown/etc.
  im Verbund), genau EIN konfiguriertes Erweiterungsgerät, nur der bestehende
  "Simultan"-Animationsmodus (keine Verbund-weite Staffelung), manuelle IP/Hostname-Kopplung,
  Fail-Soft bei Nichterreichbarkeit. Beweist das Grundprinzip mit überschaubarem Risiko.
- **Phase 2:** mehrere Erweiterungsgeräte, Animations-Koordination über Geräte hinweg (Punkt 5),
  Rotation-Modus-Unterstützung im Verbund.
- **Phase 3:** robustere Fehlerbehandlung/UX (z. B. Erweiterungsgerät-Status im Admin-Panel des
  Primärgeräts), evtl. automatische Erkennung als Komfort-Ergänzung zur manuellen Kopplung.

## Offene Entscheidungen für die spätere Umsetzungs-Session

- Exakte Zahl unterstützter Erweiterungsgeräte in Phase 1 (Vorschlag: Datenstruktur von Anfang an
  als Liste anlegen, auch wenn Phase 1 nur mit einem Eintrag getestet wird).
- Fail-Soft-Verhalten (Punkt 2 oben) explizit bestätigen oder Alternative festlegen.
- Deutsche UI-Beschriftung ("Primärgerät"/"Erweiterungsgerät") vs. Beibehaltung der englischen
  Codebegriffe auch in der Oberfläche.

## Betroffene Dateien (grober Überblick, keine Details in diesem Planungsschritt)

- `ESPMaster/ESPMaster.ino`: neue `deviceRole`-Einstellung + Erweiterungsgeräte-Liste (Laden/
  Speichern, neuer `/api/extension-display`-Endpunkt), Anpassung von `performOneSecondDisplayTick()`.
- `ESPMaster/HelpersStringHandling.ino`: `processSentenceToLines()`/`centerString()`/
  `leftString()`/`rightString()` auf Breiten-Parameter statt globaler `unitsAmount`-Variable umstellen.
- `ESPMaster/ServiceDisplayJobFunctions.ino`: Aufteilung der fertigen Zeile in lokalen + entfernte
  Abschnitte, neue Health-Check-Logik für Erweiterungsgeräte.
- `ESPMaster/data/index.html`: neue UI für Rollen-Konfiguration + Erweiterungsgeräte-Liste.

## Verifikation (für die spätere Umsetzungs-Session)

1. Zwei echte Geräte im selben Netzwerk (oder ein zweites Testgerät) nötig - nicht mit nur einem
   Gerät simulierbar.
2. Phase-1-Testmatrix: Primärgerät zeigt kombinierten Text korrekt über beide Geräte verteilt;
   Erweiterungsgerät bleibt eigenständig über seine normale Web-UI erreichbar; Erweiterungsgerät
   offline → Primärgerät zeigt weiterhin seinen eigenen Anteil (Fail-Soft bestätigt); Heap/
   Crash-Log/Unit-Status auf BEIDEN Geräten nach jedem Test unauffällig (etabliertes Muster dieser
   Session).
