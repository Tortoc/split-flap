# Mehrgeräte-Verbund (Primärgerät + Erweiterungsgerät) - Architekturplan

**Status (2026-09-16): Phase 1 implementiert UND live auf echter Hardware verifiziert -
END-TO-END, physisch vom Nutzer bestätigt.** Neue Dateien: `ServiceExtensionFunctions.ino`
(Health-Check, Push, Empfang), `Classes.h`s `ExtensionDevice`-Struct. Geänderte Dateien:
`ESPMaster.ino` (Rollen-Einstellung, Erweiterungsgeräte-Liste, neue Endpunkte
`/api/device-role`, `/api/extension-devices`, `/api/extension-display`, `deviceRole` im
Boot-Snapshot), `HelpersStringHandling.ino` (`centerString()`/`leftString()`/`rightString()`/
`processSentenceToLines()` bekommen einen `width`-Parameter statt der globalen `unitsAmount`),
`ServiceDisplayJobFunctions.ino` (nutzt `combinedUnitsAmount()` an den 5 relevanten Stellen, ruft
`pushExtensionSegments()` einmal pro neuer Zeile), `data/index.html` (neuer
"Mehrgeräte-Verbund"-Tab).

**Live-Test (2026-09-16), zwei echte Geräte:** 192.168.1.50 (9 Units) als Primärgerät, 192.168.1.51
(1 Unit) als Erweiterungsgerät konfiguriert. Health-Check erkannte 181 korrekt (`reachable:true,
unitsAmount:1`) innerhalb des ersten 15s-Zyklus. Ein 10-Zeichen-Text ("ABCDEFGHIJ", exakt die
kombinierte Breite 9+1) wurde an 195 gesendet - **vom Nutzer physisch bestätigt: 195 zeigt
"ABCDEFGHI", 181 zeigt "J"**, exakt wie erwartet. Heap/Crash-Log auf beiden Geräten während des
gesamten Tests unauffällig (kein Fehler im `HTTPClient`-basierten Health-Check/Push-Pfad). Damit
ist der komplette Phase-1-Pfad (Rollen-Konfiguration, Health-Check, kombinierte Breite,
Segment-Aufteilung, Netzwerk-Push, lokale Anzeige auf beiden Geräten) real bestätigt, nicht nur
kompiliert.

**Rotation-Modus im Verbund (2026-09-16): funktioniert bereits ohne Codeänderung.**
`ServiceRotationFunctions.ino` enthält keine einzige `unitsAmount`-Referenz - Rotation läuft
komplett über die gemeinsame `showText()`/`startDisplayJob()`-Pipeline, die Phase 1 bereits auf
`combinedUnitsAmount()` umgestellt hat. Live mit einem 10-Zeichen-Rotationseintrag getestet,
**vom Nutzer physisch bestätigt: 195 zeigt "ABCDEFGHI", 181 zeigt "J"**, genau wie im
Text-Modus-Test. Damit ist Phase 2s "Rotation-Modus-Unterstützung im Verbund"-Punkt bereits
erledigt, nicht mehr offen.

**Phase 2: Animations-Koordination über Geräte hinweg - implementiert UND live verifiziert
(2026-09-16).** `computeUnitAnimationDelayMs()` bekommt statt des lokalen Unit-Index einen
globalen Index: `pushSegmentToExtension()` sendet zusätzlich zum Segment `globalOffset` (Summe aus
eigenem `unitsAmount` und allen vorher konfigurierten Erweiterungsgeräten' `UnitsAmount`) und
`combinedWidth`; das Erweiterungsgerät berechnet damit den Stagger-Delay jeder seiner eigenen Units
mit genau derselben Formel wie das Primärgerät lokal, über eine begrenzte blockierende
Wait-Until-Due-Schleife (`ANIMATION_STAGGER_MS_MAX * kleine Unit-Anzahl` als Obergrenze - bewusste,
dokumentierte zweite Ausnahme von der Nicht-Blockierend-Regel, siehe `ServiceExtensionFunctions.ino`).
Live getestet: `animationMode=outside-in`, `animationStaggerMs=300` gesetzt, zwei 10-Zeichen-Texte
gesendet - **vom Nutzer physisch bestätigt: sieht wie EINE durchgehende Animation über beide Geräte
hinweg aus** (nicht wie zwei unabhängige lokale Animationen). Heap/Crash-Log auf beiden Geräten
während und nach dem Test unauffällig.

**Kritischer Fund während der Phase-2-Entwicklung: OTA-Update kollidiert mit aktiver
Primärgerät-Rolle (2026-09-16), behoben.** Ein Versuch, neue Firmware per OTA auf ein Gerät zu
spielen, das gerade aktiv als `deviceRole=primary` mit laufendem `deviceMode=rotation` (also
regelmäßigen blockierenden `HTTPClient`-Aufrufen aus `checkExtensionHealth()`/
`pushExtensionSegments()`) lief, ließ das Gerät nach dem ersten Chunk-Upload vollständig
einfrieren (kein Crash-Log-Eintrag - ein echtes Hängenbleiben, kein abgefangener Absturz),
Wiederherstellung nur per physischem Power-Cycle möglich. **Fix:** beide Funktionen prüfen jetzt
zu Beginn die bereits vorhandenen OTA-Status-Globals (`otaChunkBuffer != nullptr ||
otaPendingPhase != OTA_PHASE_NONE`, ursprünglich für die eigene OTA-Empfangslogik in
`ESPMaster.ino` gedacht) und überspringen den Netzwerk-Aufruf komplett, solange ein OTA-Update
läuft. Firmware mit diesem Fix wurde erfolgreich auf beide Geräte deployed (sauberer Neustart,
kein Crash-Log-Eintrag), danach die Rollen erneut konfiguriert und der Animations-Test (siehe oben)
erfolgreich auf der gefixten Firmware durchgeführt. Details siehe `BUGFIX_LOG.md`.

**Phase 3: robustere Fehler-UX im Admin-Panel - implementiert UND live verifiziert (2026-09-16).**
Nutzerentscheidung (per Rückfrage): nur die Fehler-UX umsetzen, NICHT die optionale automatische
mDNS-Erkennung (Begründung im Plan selbst: mDNS-Browsing ist auf diesem Core "deutlich
unfertiger/fragiler" als reines Ankündigen - die manuelle Kopplung bleibt bewusst bestehen).
Der "Mehrgeräte-Verbund"-Tab im System-Modal zeigt den Status jedes konfigurierten
Erweiterungsgeräts jetzt als farbige Zeile (grün "Erreichbar, N Units" / rot "Nicht erreichbar" /
gelb "wird geprüft..." - dieselben `status-online-ready`/`status-offline`/`status-online-busy`-
Klassen wie die bestehende Units-Ansicht) statt reinem Text, und aktualisiert sich automatisch alle
15s, solange der Tab sichtbar ist (Polling startet beim Öffnen des Tabs, stoppt beim Wechsel zu
einem anderen Tab oder beim Schließen des System-Modals - kein unnötiger Netzwerk-Traffic im
Hintergrund). **Vom Nutzer im Browser live bestätigt.** Reine `data/index.html`-Änderung, keine
Firmware-/Endpunkt-Änderung nötig (nutzt weiterhin das bestehende `GET /api/extension-devices`).

**Noch nicht getestet** (bewusst außerhalb des heutigen Tests, siehe Phasenplan oben):
Fail-Soft-Verhalten bei nicht erreichbarem Erweiterungsgerät (Theorie, nicht provoziert), mehrere
gleichzeitige Erweiterungsgeräte (Nutzer hat aktuell nur Material für ein Erweiterungsgerät).
Automatische Erkennung (mDNS) bleibt bewusst nicht umgesetzt (Nutzerentscheidung 2026-09-16).

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
