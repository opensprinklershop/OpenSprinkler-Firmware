# Plan: Web-UI auf dem Gerät speichern (ESP32-C5 und OSPi)

Stand 2026-10-31. Ziel: Die OpenSprinkler-App-Oberfläche wird nicht mehr von
`ui.opensprinklershop.de` nachgeladen, sondern liegt im Speicher des Controllers
(OS 4 Pro ESP32-C5: externer SPI-Flash, OSPi: Datenverzeichnis) und wird vom
Gerät selbst ausgeliefert. Die Cloud-UI bleibt als Fallback und für den ESP8266.

## 1. Ist-Zustand (gemessen)

| Punkt | Befund |
|---|---|
| Ladepfad | `server_home()` (`opensprinkler_server.cpp:1861`) liefert eine Minimalseite mit `<script src="<SOPT_JAVASCRIPTURL>/home.js">`. `home.js` lädt `vendor-js/jquery.js`, `js/hasher.js`, `versions.json`, wählt den Snapshot zur Firmware-Version (`/jo` → `fwv`/`fwm`) und lädt daraus CSS, `vendor-js/libs.js`, `dataTables`, `modules.json` + 39 Module, `js/main.js`, `vendor-js/jqm.js`, `index.html`, Locale. |
| Kern-UI (alles, was `home.js` beim Start lädt) | 2,56 MB roh, **616 KB gzip**, 512 KB brotli (unminifiziert, `ui/www`). Eigene Module minifiziert nochmal ca. 35 % kleiner → Kern ≈ **480 KB gzip**. |
| Lazy-Vendor (`lazy.js`: vis-timeline, apexcharts, chart.js, qrcode) | 1,45 MB roh, **403 KB gzip** |
| Alle 32 Sprachen | 4,1 MB roh, **1,14 MB gzip** (de allein 41 KB) |
| img + fonts + css/images | ≈ 155 KB (PNG, nicht komprimierbar) |
| **Gesamt minimal** (Kern + Lazy + Bilder + de/en) | **≈ 1,1 MB** |
| **Gesamt komplett** (alle Sprachen) | **≈ 2,2 MB** |
| C5 interner Flash | 8 MB, Slots je 0x390000 (3,56 MB). Zigbee-Image 3,01 MB (≈ 720 KB frei), Matter-Image 3,39 MB (≈ 340 KB frei). Interne `littlefs`-Partition 0x87000 = 540 KB. → **Einbetten ins Image oder interne Partition scheidet aus.** |
| C5 externer Flash | `init_external_flash()` (`main.cpp:3938`) registriert den kompletten externen SPI-Flash (SPI2, DOUT, 60 MHz, CS IO5) als Partition `littlefs_ext`; `LittleFS.begin(true,"/littlefs",10,"littlefs_ext")` mountet ihn als **das** Dateisystem. Bench .151: `/db` meldet `flash 16777216`, `used 13938688` → **16 MB, davon 13,9 MB belegt**. |
| Was den externen Flash füllt | Sensorlogs: `MAX_LOG_SIZE` = 2 097 152 **Einträge** × `sizeof(SensorLog_t)` (≈ 24 B mit `double`) je Datei, 2 Dateien × 3 Logs (`checkLogSwitchAfterWrite`, `sensors.cpp:1393`). Es gibt keine Untergrenze für freien Platz → Logs können den Flash komplett füllen. |
| HTTP-Server | OTF streamt die Antwort direkt zum Client (`enableStream`, `OpenThingsFramework.cpp:266`); ein Client zur Zeit, `Connection: close`. Gzip-Auslieferung aus PROGMEM gibt es schon (`print_header_compressed_html`, `on_ap_home`, `/update`). `onMissingPage` wird im Stationsmodus nicht genutzt (nur AP-Modus). `Request::getPath()` vorhanden. |
| OTA-Downloader (C5) | `online_update.cpp`: HTTPS (`WiFiClientSecure`, `setInsecure()`), Image komplett in PSRAM, SHA-256 aus `manifest.json`, dann Flash. ESP8266-OTA ist RSA-signiert. |
| Entpacken | Kein tar/zip/inflate im Firmware-Code. |
| UI-Release | `fw.sh release` → `buildwebdeploy.sh` promotet `ui-test/dev` nach `ui-live/www/<2.4.0.NNN>/` (unminifiziert, 9,4 MB), erzeugt signierte `filelist.json`/`filelist.sig` (ECDSA P-256, Key `App-Keys/ui-update-signing.pem`, Script `scripts/gen-ui-filelist.js`), minifiziert (`minify-dist.sh`, terser/cleancss) nur für den IONOS-Upload. |
| OSPi | Dateien liegen im Datenverzeichnis (`get_data_dir()` = Laufzeitpfad, `utils.cpp:143`), Update = `updater.sh` (`git pull`, `build.sh`, Service-Restart). Gleicher OTF-Serverpfad (`LinuxLocalServer`). |

Schlussfolgerung: Auf dem C5 ist der **externe 16-MB-Flash** der einzige
sinnvolle Ort (LittleFS, Verzeichnis `/ui/`). Voraussetzung ist eine
Platz-Reserve gegenüber den Sensorlogs. Auf dem OSPi ist es ein Verzeichnis
`ui/` neben den Datendateien.

## 2. Zielbild

```
Gerät                                   Server (opensprinklershop.de/upgrade)
/                → server_home: <script src="ui/home.js">     manifest.json  +ui_version, ui_url,
/ui/home.js      ─┐                                                          ui_sha256, ui_sig, ui_size
/ui/versions.json │ aus LittleFS /ui/<ver>/… (vorab gzip)      ui_2.4.0.NNN.tar  (ustar, Dateien einzeln .gz)
/ui/<ver>/…      ─┘ Cache-Control: immutable
/ul              Status / Installation anstoßen / entfernen (JSON)
/uf              POST Upload des Pakets (Offline-Installation, wie /update)
```

- **Paketformat**: unkomprimiertes **ustar**-Archiv, darin jede Datei bereits
  `gzip -9` (`js/main.js.gz`, …), Bilder/Fonts roh, plus `ui.json`
  (`{"version":"2.4.0.231","files":N,"bytes":…}`). Gründe: streambar (512-Byte-Header,
  kein Index am Ende), ein Tar-Leser ist ~80 Zeilen C++, OSPi und Entwickler
  können `tar x` nehmen, keine Inflate-Bibliothek auf dem Gerät nötig.
- **Inhalt** (Build-Option): Kern + Lazy-Vendor + img/fonts + Locales.
  Standard: alle Sprachen (≈ 2,2 MB). Module werden im Paket zu **einer**
  `js/modules/bundle.js` zusammengefasst und `modules.json` zeigt nur darauf →
  statt ≈ 60 Requests ≈ 15 (wichtig, weil OTF einen Client zur Zeit bedient).
- **Authentizität**: ECDSA-P-256-Signatur über den SHA-256 des Pakets mit dem
  vorhandenen UI-Update-Key (`App-Keys/ui-update-signing.pem`); Public Key
  wird in die Firmware kompiliert (mbedtls `ecdsa` ist durch TLS schon drin).
  Ohne gültige Signatur wird nichts aktiviert, auch nicht bei Upload über `/uf`.
  Grund: der OTA-Client läuft mit `setInsecure()`, das Manifest allein beweist
  keine Herkunft; EN 18031 (SUM/SCM) verlangt geprüften Software-Update-Pfad.
- **Atomarer Wechsel**: Download/Upload → `/ui.new/<ver>/…` (SHA-256 wird beim
  Streamen mitgerechnet, kein PSRAM-Puffer, läuft so auch auf OSPi und in der
  Matter-Variante) → Prüfung → `/ui` nach `/ui.old` umbenennen, `/ui.new` nach
  `/ui`, `/ui.old` rekursiv löschen. Marker `/ui/ui.json` = „gültig installiert“.
- **Auswahl der Quelle** (`IOPT_UI_SOURCE`, neu): 0 = automatisch (lokal wenn
  installiert, sonst Cloud), 1 = nur lokal, 2 = nur Cloud. `server_home()`
  entscheidet damit; `SOPT_JAVASCRIPTURL` bleibt für Cloud/Fallback erhalten.
  `/jo` liefert zusätzlich `uiv` (installierte UI-Version oder leer) und `uis`
  (Quelle), damit die App den Zustand zeigen kann.
- **Caching**: Pfade unter `/ui/<ver>/` bekommen `Cache-Control: public,
  max-age=31536000, immutable`; `/ui/home.js` und `/ui/versions.json` `no-cache`.
  Nach einem UI-Update ändert sich `<ver>` im Pfad → kein Cache-Problem, zweiter
  Seitenaufruf lädt praktisch nichts mehr vom Gerät.
- **home.js**: eine kleine Ergänzung: enthält `versions.json` `"local": true`,
  wird `assetLocation = rootLocation + default + "/"` ohne Cloud-Logik gesetzt
  (gleiche Datei für Cloud und Gerät; `rootLocation` ergibt sich aus der
  Script-URL `…/ui/home.js`, relative Pfade funktionieren auch hinter dem
  OTC-Relay-Präfix `/forward/v1/<token>/`).

## 3. Arbeitspakete

### AP0 – Vorarbeit: Platz-Reserve auf dem externen Flash (C5), 0,5 Tag
- `checkLogSwitchAfterWrite()`/`sensorlog_add()`: zusätzlich zur Eintragsgrenze
  eine **Untergrenze freier Platz** (z. B. 3 MB auf ESP32 mit `littlefs_ext`,
  512 KB sonst): ist sie unterschritten, wird das größte Log umgeschaltet und
  die alte Datei gelöscht, bevor geschrieben wird.
- `MAX_LOG_SIZE` (2 M Einträge) realistisch deckeln (z. B. 256 k Einträge ≈ 6 MB
  je Datei) – Stefan entscheidet die Zahl; Verhalten in `CHANGELOG` dokumentieren.
- Messen am Bench .151 (13,9 MB belegt): nach dem Fix muss `/db` ≥ 3 MB frei
  zeigen, sonst scheitert jede UI-Installation reproduzierbar.

### AP1 – Paket-Builder und Signatur (Server-Seite), 1 Tag
- `tools/mkuipack.py` (im Firmware-Repo, Python, ohne Fremdpakete):
  Eingabe `ui-live/www/<ver>/` (nach `minify-dist.sh`), Ausgabe
  `/data/upgrade/ui_<ver>.tar` + SHA-256 + Signatur.
  Schritte: Dateiauswahl (Kern, Lazy-Vendor, img, fonts, css/images, locale;
  Ausschlüsse wie in `gen-ui-filelist.js`: cordova, plugins, sw.js, *.bak, *.md),
  Module nach `js/modules/bundle.js` konkatenieren und `modules.json` ersetzen,
  `gzip -9` je Textdatei, `ui.json` schreiben, ustar schreiben, `openssl dgst
  -sha256 -sign` mit dem UI-Key (DER → raw r||s 64 Byte, base64).
- `fw.sh release`: nach der UI-Promotion `mkuipack.py` aufrufen,
  `manifest.json`/`versions.json` um `ui_version`, `ui_url`, `ui_sha256`,
  `ui_sig`, `ui_size` ergänzen (auch Beta-Kanal). Paket wird mit dem
  `upgrade/`-Baum zu IONOS gespiegelt (bestehender `online_deploy`).
- Lokaler Test: Paket per `tar t` prüfen, Signatur mit `openssl dgst -verify`.

### AP2 – Firmware: Auslieferung aus dem Dateisystem (C5 + OSPi), 1,5 Tage
- Neues Modul `ui_local.cpp/.h` (plattformneutral, Datei-I/O über `LittleFS`
  auf ESP32 bzw. `fopen` auf OSPi, wie `utils.cpp`):
  - `ui_local_installed_version(buf)` liest `/ui/ui.json`.
  - `ui_local_serve(req,res)`: Pfad aus `req.getPath()`, Whitelist
    `[A-Za-z0-9._/-]`, kein `..`, Länge ≤ 96; `/ui/home.js` und
    `/ui/versions.json` (dynamisch erzeugt: `{"versions":["<ver>"],"default":
    "<ver>","local":true}`) gesondert; sonst `/ui/<ver>/<rest>`; existiert
    `<rest>.gz` → `Content-Encoding: gzip`. MIME nach Endung (js, css, html,
    json, png, ico, svg, woff, woff2, ttf, map). Streaming in 4-KB-Blöcken
    (Puffer transient aus PSRAM/Heap, kein neues statisches Array) über
    `res.writeBodyData()`; `Content-Length` aus Dateigröße; 404 sonst.
- `opensprinkler_server.cpp`: im Stationsmodus `otf->onMissingPage(on_missing)`
  registrieren, das `/ui/`-Präfix an `ui_local_serve` gibt und sonst wie bisher
  404 antwortet. `server_home()`: bei lokaler Quelle
  `<script src="ui/home.js">` statt `$O/home.js`. CORS-Header wie bei
  `print_header` (die Seite ist same-origin, Header schadet nicht).
- `defines.h`: `IOPT_UI_SOURCE` anhängen (Default 0), Default in
  `options_setup()`, `jo`/`co` in `opensprinkler_server.cpp`; `/jo` um `uiv`,
  `uis` erweitern.
- Messung auf dem Bench .151 (WiFi und W5500): Zeit bis „App geladen“ bei
  kaltem und warmem Browser-Cache, Heap vor/nach; Vergleich mit Cloud-UI.
  Ziel: < 8 s kalt über WiFi, keine Heap-Regression > 2 KB im Leerlauf.

### AP3 – Firmware: Installation (Download, Upload, Prüfung), 2 Tage
- `ui_local_install_begin(ver, expected_sha256, sig)` / `_write(buf,len)` /
  `_finish()`: Tar-Streaming-Parser (ustar-Header, Name + `prefix`, Größe
  oktal, 512er-Padding), schreibt nach `/ui.new/<ver>/…`, rechnet
  `mbedtls_sha256_update` über den Rohstrom mit; `_finish()` vergleicht Hash,
  prüft ECDSA (`mbedtls_ecdsa_read_signature` mit kompiliertem Public Key,
  Signatur aus `ui_sig` in raw r||s → DER wandeln oder `mbedtls_ecdsa_verify`
  mit `mpi` direkt), schaltet atomar um, räumt `/ui.old` rekursiv ab.
  Fehler jeder Art → `/ui.new` löschen, alte UI bleibt aktiv.
- Platzprüfung vor Beginn: `frei ≥ ui_size + 512 KB`, sonst Fehlerstatus
  „not enough flash“ (mit Zahlen) statt Teilinstallation.
- **Download (C5)**: eigener FreeRTOS-Task wie `ota_update_task` (nie aus dem
  Main-Loop, siehe Regel zu `send_http_request`), `HTTPClient` + `WiFiClientSecure`,
  Stream in 4-KB-Blöcken an `_write()`; Zustandsmaschine/Fortschritt analog
  `ota_set_state()` (eigener `OnlineUpdateStatus`-Bereich oder eigenes Status-
  Struct, damit `/jo`/App den Fortschritt zeigen). Nicht parallel zur Firmware-OTA.
- **Download (OSPi)**: gleicher Installer, Download mit libcurl (bereits gelinkt,
  `-lcurl`) in einem `std::thread`; alternativ liest `updater.sh` das Paket mit
  `curl` nach `<data_dir>/ui_<ver>.tar` und die Firmware installiert beim Start
  aus der lokalen Datei. Empfehlung: Laufzeit-Download in der Firmware, damit
  `/ul?install=1` auf beiden Plattformen gleich funktioniert.
- **Upload** `/uf` (POST multipart auf `update_server`, wie `/update`): für
  Geräte ohne Internet; Signaturprüfung identisch, `sha256` kommt dann aus dem
  Paket-Trailer bzw. wird nur über die Signatur geprüft (Signatur deckt den Hash).
- **Auslöser**: (a) `/ul?install=1[&url=…]` manuell, (b) nach erfolgreicher
  Firmware-OTA in `ota_phase2_task`, wenn das Manifest `ui_version` nennt,
  (c) beim Boot einmal täglich prüfen (`online_update_check_safe` liefert das
  Manifest schon): installierte UI-Version ≠ `ui_version` **und** passend zur
  Firmware (`ui_version` beginnt mit `fwv.fwm`) → Installation, sofern
  `IOPT_UI_SOURCE` ≠ 2. Fehlschlag → Cloud-Fallback bleibt, Retry mit Backoff.
- Endpoints: `/ul` GET → `{"installed":"2.4.0.231","active":1,"source":0,
  "state":…, "progress":…, "msg":…, "free":…}`; `/ul?install=1`, `/ul?remove=1`
  (löscht `/ui`, Quelle fällt auf Cloud zurück). Passwortpflichtig wie `/cv`.

### AP4 – App/UI, 0,5 Tag (Repo `/srv/www/htdocs/ui`)
- `home.js`: `"local": true`-Zweig (siehe oben), sonst unverändert.
- Einstellungen („System“/„Firmware“-Seite): Quelle wählen (automatisch/lokal/
  Cloud), installierte UI-Version, Button „UI auf Gerät aktualisieren“ mit
  Fortschritt aus `/ul`; Hinweis, wenn der freie Flash nicht reicht.
- `firmware.js` (Update-Dialog): nach Firmware-Update zeigt der Status auch die
  UI-Installation.

### AP5 – OSPi-Integration, 0,5 Tag
- `build.sh`/`updater.sh`: nichts Zwingendes, wenn der Laufzeit-Download in
  der Firmware läuft. Optional: `updater.sh` legt das Paket vorab ab.
- `data_dir` erweitern: `ui/`-Verzeichnis mit korrekten Rechten (Service läuft
  als root, unkritisch). Test auf dem Bench-Pi (`OSPI_PI_HOST` in `.env`).

### AP6 – Doku, Release, CE, 0,5 Tag
- `docs/docs/pro-api-endpoints.md` und
  `docs/as_api_docs/api_endpoint_platform_matrix.md`: `/ul`, `/uf`, `/ui/*`,
  `/jo` `uiv`/`uis`, `IOPT_UI_SOURCE` (ESP32 ✔, OSPi ✔, ESP8266 ✘).
- `CHANGELOG.md` (deutsch) unter 2.4.0(231).
- CE-Ordner `06_EN18031_Selbstbewertung.md`: zweiter signierter Update-Pfad
  (UI-Paket) ergänzen; Schlüsselverwaltung = bestehender UI-Update-Key.
- Memory/CLAUDE.md: externer 16-MB-Flash als LittleFS, `/ui`-Layout, Log-Floor.

## 4. Risiken und offene Entscheidungen

1. **Flash voll durch Sensorlogs** (Bench: 13,9/16 MB). Ohne AP0 funktioniert
   nichts zuverlässig. Entscheidung: Log-Obergrenze pro Datei und Floor.
2. **Ladezeit über einen seriellen HTTP-Server.** OTF bedient einen Client zur
   Zeit; Browser öffnen 6 Verbindungen, der Rest wartet im lwIP-Backlog. Mit
   dem Modul-Bundle sind es ≈ 15 Requests, ≈ 1,1 MB; Schätzung 3–6 s über WiFi,
   danach Cache. AP2 misst das vor AP3; fällt es schlecht aus: Backlog des
   `WiFiServer` erhöhen oder OTF-Multiclient (es gibt `MULTICLIENT_GUIDE.md` im
   OTF-Fork, aktuell nicht aktiv) prüfen.
3. **Matter-Variante**: interner RAM knapp. Deshalb Streaming-Installer ohne
   PSRAM-Vollpuffer und Auslieferung in 4-KB-Blöcken; Heap vor/nach messen.
4. **Versionskopplung**: UI-Paket trägt die Firmware-Version im Namen
   (`ui_2.4.0.231.tar`); ein Paket passt genau zu einer Firmware. Hotfix der
   UI ohne Firmware-Release = neues Paket mit Suffix (`2.4.0.231-2`) und
   `ui_version` im Manifest; `home.js` nutzt ohnehin nur `default`.
5. **Alle Sprachen (2,2 MB) oder Auswahl (1,1 MB)?** Empfehlung: alle, die 16 MB
   geben es her, und ein Gerät ohne Internet soll die eingestellte Sprache haben.
6. **AP-/Setup-Modus** bleibt bei der eingebetteten `ap_home.html`; die lokale UI
   wird erst im Stationsmodus ausgeliefert.
7. **ESP8266**: bleibt Cloud (kein Platz, 1 MB FS), `IOPT_UI_SOURCE` dort fest 2.
8. **HTTPS-Server des Geräts** (`Esp32HttpsClient`): gleicher Pfad, keine
   Sonderbehandlung; Mixed-Content-Probleme entfallen sogar, weil alles same-origin ist.

## 5. Reihenfolge und Aufwand

AP0 (0,5) → AP1 (1) → AP2 (1,5, enthält Messung als Go/No-Go) → AP3 (2) →
AP4 (0,5) → AP5 (0,5) → AP6 (0,5). Summe ≈ 6,5 Tage. Ab AP2 ist ein manuell
per `tar`/Script aufgespieltes Paket auf dem Bench .151 nutzbar; Release
frühestens mit 2.4.0(231), weil `IOPT_*` und das Manifest-Format erweitert werden.

## 6. Variante: UI an das Firmware-Image anhängen (Stefan, 2026-10-31)

Idee: `firmware_zigbee.bin` / `firmware_matter.bin` bekommen das UI-Tar als
Anhang; der OTA-Pfad schreibt den App-Teil in den Slot und entpackt den Rest in
den externen Flash. Technisch möglich und als **Haupttransport für den C5-OTA**
sinnvoll; der Tar-Installer aus AP3 bleibt derselbe, nur die Quelle ändert sich.

Warum es funktioniert:
- Das ESP-App-Image kennt seine eigene Länge (`esp_image_get_metadata()` →
  `image_len`, inkl. Prüfsumme und angehängtem SHA-256). Bootloader und
  `esp_ota_*` ignorieren Bytes dahinter, solange nur `image_len` Bytes in den
  Slot geschrieben werden.
- Trailer-Format hinter dem Image: `"OSUI"` + u32 Länge + u8[32] SHA-256 des Tars
  + Signatur (64 B, ECDSA P-256) + Tar. Das Manifest-`sha256` deckt weiter die
  ganze Datei; die Signatur sichert den UI-Teil zusätzlich ab.

Was dafür geändert werden muss (ersetzt in AP3 den separaten Download):
1. `online_update.cpp` C5-Pfad auf **Streaming** umstellen: Header (24 B +
   erweiterter Header) parsen, `image_len` bestimmen, die ersten `image_len`
   Bytes per `esp_ota_write()` in den Slot, alles danach an
   `ui_local_install_write()`. Grund: heute wird das ganze Image in PSRAM
   gepuffert und gegen `target->size` (3,56 MB) geprüft (`online_update.cpp`
   ≈ Zeile 450); 3,4 MB Matter-Image + 2,2 MB UI = 5,6 MB würden an der
   Slot-Prüfung scheitern und mit 8 MB PSRAM neben dem Matter-Stack knapp.
   Der SHA-256 des Manifests wird beim Streamen mitgerechnet; stimmt er am Ende
   nicht, werden `esp_ota_abort()` und `/ui.new`-Löschung ausgeführt (der alte
   Slot bleibt Boot-Slot, die alte UI aktiv).
2. `/update`-Upload (`on_firmware_upload`, `Update.begin/write`): denselben
   Splitter davor schalten, sonst schreibt `Update` den Anhang in den Slot
   (Zigbee hätte 720 KB Luft, Matter 340 KB → Überlauf → `Update` bricht ab).
3. `fw.sh`: pro Variante zwei Artefakte erzeugen: `firmware_<v>.bin` (rein, für
   esptool/USB und `fw.sh deploy`) und `firmware_<v>_ui.bin` (mit Anhang, für
   OTA). esptool kennt den Trailer nicht und würde ihn in den Nachbar-Slot
   schreiben. `manifest.json`: `zigbee_url`/`matter_url` zeigen auf die
   `_ui`-Dateien, `sha256` entsprechend; Fallback-Feld `*_plain_url` optional.
4. Firmware mit Anhang, aber alter Bootloader/alte OTA-Logik (Geräte < 231):
   Alte Firmware prüft `imgSize > target->size` und lehnt die 5,6-MB-Datei ab →
   Geräte vor 2.4.0(231) könnten nicht mehr online updaten. Deshalb im
   Manifest **beide** URLs führen und die alte Firmware weiter die reine Datei
   bekommen (sie liest nur die bekannten Felder; neue Firmware bevorzugt `_ui`).

Was die Variante nicht löst (deshalb bleiben `/ul`, `/uf` und das separate
`ui_<ver>.tar` im Plan, nur kleiner priorisiert):
- **USB-/esptool-Flash** und `fw.sh deploy` liefern keine UI; das Gerät muss sie
  nachladen (Download des `ui_<ver>.tar` oder der `_ui`-Datei mit Überspringen
  des App-Teils) oder bleibt auf Cloud.
- **OSPi** hat kein Firmware-Image; dort bleibt das Tar (Download zur Laufzeit
  oder `updater.sh`).
- **UI-Hotfix ohne Firmware-Release** geht nur über das separate Tar.
- Matter-Variante: Download wird mit UI etwa 65 % größer (3,4 → 5,6 MB), die
  OTA-Dauer über WiFi steigt entsprechend.

Empfehlung: Variante übernehmen als AP3a (Streaming-OTA mit Splitter, 1,5 Tage
statt 2) und AP3b (separates Tar über `/ul`/`/uf`, 1 Tag) dahinter. Gesamtaufwand
bleibt ≈ 6,5 Tage, das Nutzererlebnis beim normalen OTA ist aber „ein Update,
UI inklusive“, und UI und Firmware können nicht mehr auseinanderlaufen.

### 6.1 Akzeptiert eine alte Firmware (z. B. 2.4.0(221)) die Datei mit Anhang?

Geprüft an `git show v240_221:online_update.cpp` (gilt unverändert bis 230):

- Download: `imgSize = http.getSize()` (Content-Length der ganzen Datei),
  `heap_caps_malloc(imgSize, SPIRAM)`, SHA-256 über die ganze Datei gegen das
  Manifest. Bis hierhin wäre ein Anhang unschädlich.
- Danach harte Prüfung: `if (imgSize > target->size) → "Image too large"`,
  Abbruch. `target->size` = 0x390000 = 3 735 552 Byte.
- Besteht die Prüfung, schreibt `esp_ota_write()` die **ganze** Datei in den
  Slot (Trailer landet hinter dem Image, `esp_ota_end()` prüft nur `image_len`
  und ignoriert den Rest). Der Anhang würde also installiert, aber nie entpackt.

Folge: 221–230 akzeptieren die Datei nur, wenn Image + Anhang in den Slot
passen: Zigbee ≤ ≈ 720 KB Anhang, Matter ≤ ≈ 340 KB. Mit 1,1–2,2 MB UI lehnt
jede alte Firmware das Update ab („Image too large“) und bleibt auf ihrem Stand.
221 liest aus dem Manifest nur `zigbee_url`/`matter_url` (`manifest.valid`
verlangt beide); unbekannte Felder werden ignoriert.

Konsequenz für „Dateinamen bleiben gleich“:
- `firmware_zigbee.bin`/`firmware_matter.bin` unter `zigbee_url`/`matter_url`
  müssen **rein** bleiben, solange Geräte < 231 im Feld sind (de facto dauerhaft).
- Die Datei mit Anhang bekommt ein eigenes Manifest-Feld (`zigbee_ui_url`,
  `matter_ui_url`, `*_ui_sha256`); neue Firmware bevorzugt es, alte sieht es nicht.
- Migration: 221 → 231 kommt als reines Image an, hat also noch keine lokale
  UI. Die holt sich 231 dann einmalig über den separaten Tar-Pfad (AP3b, `/ul`
  automatisch beim ersten Manifest-Check) und ab dem nächsten OTA über den
  Anhang. AP3b ist damit nicht optional, sondern Teil der Migration.
- Nicht empfehlenswert: Serverseitig (IONOS) je nach Query/User-Agent
  unterschiedliche Inhalte unter demselben Namen ausliefern; das bricht
  Hash-Prüfung, Spiegel und Nachvollziehbarkeit.

### 6.2 Konsistenzregeln Firmware ↔ lokale UI

Grundsatz: **Eine lokale UI wird nur ausgeliefert, wenn ihre Version exakt der
laufenden Firmware entspricht** (`ui.json.version == "2.4.0.<OS_FW_MINOR>"`).
Alles andere gilt als „keine lokale UI“ → Cloud-Fallback, der die Versionswahl
über `versions.json` ohnehin beherrscht. Es gibt keinen Zustand, in dem das
Gerät stillschweigend eine fremde UI-Version zeigt.

Ablage pro Version statt eines einzigen `/ui`-Baums:

```
/ui/2.4.0.231/…   /ui/2.4.0.232/…   (+ /ui/<ver>/ui.json mit version, build, sha256, Quelle)
```

Regeln:
1. **Auswahl**: `server_home()` sucht `/ui/<fwv.fwm>/ui.json`. Treffer → lokal,
   sonst Cloud (bei `IOPT_UI_SOURCE = auto`).
2. **232 ohne UI kommt an** (USB/esptool, `/update` mit reiner Datei, reine
   OTA-URL): Beim Boot findet 232 nur `/ui/2.4.0.231` → Cloud-Fallback sofort,
   die Oberfläche funktioniert weiter. Der tägliche Manifest-Check holt dann
   `ui_2.4.0.232.tar` (Konvention `upgrade/ui/ui_<ver>.tar`, bleibt für jede
   veröffentlichte Version auf dem Server liegen, wie die Snapshots in
   `ui-live/www/<ver>`), installiert nach `/ui/2.4.0.232/` und schaltet um.
   `/jo` meldet währenddessen `uiv:"2.4.0.231"`, `uis:2` (Cloud, Grund
   „mismatch“), die App kann darauf hinweisen.
3. **OTA mit Anhang**: Reihenfolge App-Teil in den Slot → UI nach
   `/ui/2.4.0.232/` → beide geprüft → erst dann `esp_ota_set_boot_partition()`
   + Reboot. Scheitert die UI-Prüfung, wird der OTA-Vorgang insgesamt
   abgebrochen (`esp_ota_abort()`, Verzeichnis löschen) – kein „Firmware neu,
   UI alt“ durch einen halben Erfolg.
4. **Rollback**: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; der Arduino-Core
   bestätigt das Image beim Erreichen von `initArduino()`. Fällt der
   Bootloader auf 231 zurück, liegt `/ui/2.4.0.231` noch da (Regel 5) → konsistent.
5. **Aufräumen**: Fremde Versionsverzeichnisse werden erst gelöscht, wenn die
   laufende Firmware 10 Minuten stabil läuft **und** mehr als zwei Versionen
   liegen oder der freie Platz unter die Reserve fällt. Standard: aktuelle +
   vorherige Version bleiben (≈ 2 × 2,2 MB). Grund: die beiden OTA-Slots
   (Zigbee/Matter, `fw.sh switch`) können verschiedene Versionen tragen und
   finden so beide ihre UI; die UI selbst ist variantenneutral.
6. **Downgrade** (232 → 231 über `/update`): identisch zu Regel 2; liegt
   `/ui/2.4.0.231` noch (Regel 5), ist er sofort lokal.
7. **Beta/Dev-Builds** mit gleicher `OS_FW_MINOR` aber anderem Code: Die
   Kopplung über die Nummer greift nicht. Für angehängte UI wird deshalb
   zusätzlich `esp_app_get_elf_sha256()` des Images in `ui.json.build` gespeichert
   und beim Auswählen verglichen (nur ESP32). Stimmt die Nummer, aber nicht der
   Build-Hash → Cloud. Separat installierte Tars (`/ul`, `/uf`) tragen keinen
   Build-Hash und gelten für jede Firmware dieser Nummer. `fw.sh deploy` kann
   nach dem Flashen die aktuelle `ui-test/dev` als Tar per `/uf` nachschieben.
8. **Nur lokal erzwungen** (`IOPT_UI_SOURCE = 1`, Gerät ohne Internet): Bei
   Versions-Mismatch darf die **nächstältere** vorhandene UI ausgeliefert werden
   (die Firmware-API ist additiv, ältere UI auf neuerer Firmware ist der Normalfall
   der Cloud mit `default`), die App zeigt dann dauerhaft den Hinweis
   „UI 2.4.0.231 auf Firmware 2.4.0.232 – UI aktualisieren“. Eine **neuere** UI
   auf älterer Firmware wird nie ausgeliefert.
9. **Integrität zur Laufzeit**: `ui.json` enthält Dateianzahl und Gesamtgröße;
   fehlt beim Start eine Datei oder stimmt die Größe nicht (abgebrochenes
   Löschen, Flash-Fehler), gilt die Version als ungültig → Cloud, Neuinstallation
   beim nächsten Check.

Damit ist der Zustand immer einer von drei: *lokal, exakt passend* – *Cloud,
weil nichts Passendes da* – *lokal erzwungen mit sichtbarem Hinweis*. Ein
Update kann die UI nie in einen undefinierten Zwischenzustand bringen, weil
Verzeichnisse nur komplett geprüft erscheinen und nie in place überschrieben werden.

### 6.3 Nutzer mit eigener UI auf einem lokalen Server (eigene `jsp`-URL)

Heute: `SOPT_JAVASCRIPTURL` (`/su?jsp=…`) zeigt auf einen beliebigen Server,
der den `ui-live`-Baum spiegelt (home.js, versions.json, Snapshots). Diese
Nutzer haben die URL bewusst gesetzt (Offline-Netz, eigene Anpassungen,
Upstream-Host `ui.opensprinkler.com`). Regel: **eine bewusst gesetzte URL ist
eine Entscheidung und wird von einem Firmware-Update nicht umgestoßen.**

Umsetzung:
1. `IOPT_UI_SOURCE` bekommt drei klar benannte Werte:
   `0` = Gerät (lokal, wenn passend installiert, sonst externe URL),
   `1` = nur Gerät (Regel 8 aus 6.2),
   `2` = externe URL (`jsp`, egal ob unsere Cloud oder ein eigener Server).
   In der App heißt das: „UI-Quelle: Gerät (empfohlen) / Externer Server“ mit
   dem bestehenden URL-Feld; `DEFAULT_JAVASCRIPT_URL` ist nur der Vorgabewert
   des Feldes. Es gibt keinen Sonderfall „unsere Cloud“ mehr.
2. **Migration beim ersten Start von 231** (einmalig, Marker in `iopts`):
   `jsp == DEFAULT_JAVASCRIPT_URL` → Quelle 0 (Gerät). `jsp` abweichend →
   Quelle 2 (extern). Wer einen eigenen Server betreibt, merkt vom Update
   nichts; wer nie etwas geändert hat, bekommt die Geräte-UI.
3. **Ausfall-Fallback** für Quelle 2 (optional, Haken „bei Ausfall Geräte-UI
   verwenden“, Default an): `server_home()` gibt
   `<script src="<jsp>/home.js" onerror="…">` aus; schlägt das Laden fehl
   (Server aus, Internet weg), lädt der Inline-Handler `ui/home.js` vom Gerät,
   sofern eine passende Version installiert ist. Das deckt auch den Cloud-Fall
   „Internet ausgefallen“ ab und ist ein Argument, das Paket auch dann zu
   installieren, wenn die Quelle extern ist (Platz vorausgesetzt).
   Hinweis: `onerror` feuert nur bei Netz-/HTTP-Fehlern des Script-Tags,
   nicht bei einem Server, der Müll liefert; das ist akzeptabel.
4. **Eigene UI auf das Gerät laden**: Die Signaturpflicht (6.2/AP3) bleibt
   bestehen, `/uf` nimmt nur von uns signierte Pakete. Wer eine veränderte UI
   will, hostet sie weiter extern (wie heute) – das ist die bewusste Trennung
   zwischen „Software-Update des Produkts“ (EN 18031 SUM: Authentizität
   geprüft) und „Nutzer verweist auf fremde Inhalte“ (Browser lädt sie, das
   Gerät prüft nichts). `tools/mkuipack.py` wird trotzdem veröffentlicht, damit
   das Paketformat offen ist; ein späterer Entwicklermodus (unsignierte Pakete
   nur nach Passwort **und** physischer Bestätigung am Gerät, deutlich
   markiert in `/jo`) ist denkbar, aber nicht Teil dieses Plans.
5. CORS: `cors_refresh_jsp_host()` bleibt an `jsp` gebunden; bei Quelle 0/1 ist
   die UI same-origin, CORS spielt dann keine Rolle.
6. Dokumentation: Abschnitt in `docs/docs` („UI-Quelle“) mit den drei Werten,
   Migrationsverhalten und dem Hinweis, dass eigene Server den kompletten
   `ui-live`-Baum (inkl. `versions.json`) spiegeln müssen – unverändert zu heute.

### 6.4 Die `/su`-Seite (Firmware-eigenes Formular) erweitern

Ist: `server_view_scripturl()` (`opensprinkler_server.cpp`, Suche
„Output script url form“) gibt ein statisches Formular aus: Textfeld `jsp`,
Textfeld `wsp`, Passwort, Submit auf `/cu`. GET ist ohne Passwort erreichbar.
`hasher.js` kommt fest von `ui.opensprinkler.com` (ohne Internet wird das
Passwort unverschlüsselt gesendet und abgelehnt). Die App nutzt `/su` nur noch
in `weather.js` als Scrape-Fallback für `wsp` (Regex passt nicht einmal auf
die aktuelle Markup-Variante mit einfachen Anführungszeichen; der Primärweg
ist `settings.wsp` aus `/jo`) und in `ui-dom.js` für den Legacy-Button
`#downgradeui` (rayshobby-URL).

Soll (Teil von AP2, Firmware-HTML ohne externe Abhängigkeiten):

```
UI-Quelle   (•) Gerät            installiert: 2.4.0.231 (2,1 MB)   [Aktualisieren] [Entfernen]
                                 — oder —  „nicht installiert“      [Jetzt installieren]
            ( ) Externer Server  [ https://ui.opensprinklershop.de/js            ] [Standard]
                                 [x] bei Ausfall des Servers Geräte-UI verwenden
Wetter      [ https://weather.opensprinklershop.de/…                             ] [Standard]
Passwort    [ ******** ]  [Speichern]
Status      Quelle aktiv: Gerät · freier Flash: 4,8 MB · letzte UI-Installation: ok
```

- Formular sendet weiter an `/cu`, neue Parameter `uis` (0 Gerät/auto, 2
  extern; 1 „nur Gerät“ nur über `/co`, nicht im Formular) und `uifb` (0/1,
  Ausfall-Fallback). `server_change_scripturl()` validiert wie bisher
  (`valid_script_url`) und schreibt `IOPT_UI_SOURCE`/`IOPT_UI_FALLBACK`.
  Beides zusätzlich generisch über `/jo`/`/co`, damit die App nicht scrapen muss.
- Radio „Gerät“ ist nur wählbar, wenn `/ui/<fwv.fwm>/ui.json` existiert;
  sonst steht dort „nicht installiert“ und der Button löst `/ul?install=1`
  aus. Der Fortschritt wird per `fetch('/ul?pw=…')` alle 2 s nachgeladen
  (Zustand/Prozent/Meldung aus AP3), ohne Seitenwechsel. „Entfernen“ →
  `/ul?remove=1` mit Rückfrage. Alle Aktionen sind passwortpflichtig; das
  Formular-Passwortfeld wird dafür mitverwendet.
- `hasher.js` (5 KB, MD5) wird in die Firmware eingebettet
  (`html/hasher.js` → `htmls.h` über `compress_htmls.mjs`, ausgeliefert als
  `/js/hasher.js` gz). Damit funktioniert `/su` ohne Internet und unabhängig
  von der UI-Quelle; der feste `ui.opensprinkler.com`-Verweis verschwindet.
- Markup so lassen, dass `weather.js` nichts Neues bricht (Feld `name=wsp`
  bleibt), aber die App auf `/jo`-Felder umstellen: `uis`, `uiv`, `uifb`,
  `wsp` sind dort vorhanden; `#downgradeui` (rayshobby) entfernen.
- Sicherheit (EN 18031): GET `/su` zeigt heute URLs ohne Passwort. Die neue
  Statuszeile (Flash-Größe, UI-Version) ist unkritisch, aber die Seite sollte
  bei `pwdef == 0` keine Aktionen ohne gültiges Passwort anbieten (ist durch
  `/ul`/`/cu` ohnehin erzwungen). Keine neue unauthentifizierte Information
  außer der UI-Version.
- App-Seite (AP4): dieselbe Auswahl in „System → Erweitert“ (heute nur das
  URL-Feld), mit Fortschrittsbalken aus `/ul`; `/su` bleibt die
  app-unabhängige Notfallseite.

Aufwand: in AP2 enthalten (+0,25 Tag für Einbettung von hasher.js und
Fortschrittsanzeige), AP4 unverändert.

## 7. Speicherregeln für die Umsetzung (verbindlich)

Interner SRAM des C5 ist knapp (Bench .151: `heap` ≈ 38 KB frei), die
Matter-Variante noch enger. Die letzten Releases waren Speicherarbeit; dieses
Feature darf nichts davon zurückdrehen.

1. **Keine neuen statischen oder globalen Puffer.** Kein `static char buf[…]`,
   kein globales Array, kein statisches `String`. Alles, was das Feature an
   Speicher braucht, wird transient geholt und wieder freigegeben:
   - Datei-Streaming-Puffer für `/ui/*` (4 KB): pro Request `heap_caps_malloc`
     (PSRAM, `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`; fällt es fehl, 1 KB intern)
     und im selben Handler `free`. Kein Puffer überlebt den Request.
   - Tar-Parser: Header (512 B) und Schreibpuffer (4 KB) in PSRAM, nur
     während der Installation. Der `mbedtls_sha256_context` (≈ 200 B) liegt in
     einer heap-allokierten Install-Struktur, die `_begin()` anlegt und
     `_finish()`/Abbruch freigibt.
   - `esp_ota_write()` braucht einen Quellpuffer im internen RAM (Cache-Freeze,
     siehe Kommentar in `online_update.cpp`): dafür den bestehenden `io_buf`
     (4 KB, `MALLOC_CAP_INTERNAL`) des OTA-Tasks weiterverwenden, keinen zweiten.
   - Pfad-/MIME-Behandlung mit Stack-Puffern ≤ 128 B; der Request-Pfad wird
     aus `req.getPath()` referenziert, nicht kopiert.
2. **Keine neuen Tasks im Leerlauf.** Der Download läuft wie die OTA in einem
   Task, der erst bei Bedarf erzeugt wird (Stack in PSRAM, wo der Core es
   zulässt, sonst minimal dimensioniert und gemessen) und sich nach Abschluss
   selbst löscht. Zustand/Fortschritt: ein kleines POD-Struct (< 100 B) im
   bestehenden OTA-Zustandsbereich, keine `String`-Member.
3. **Kein zweiter HTTP-Client parallel.** UI-Installation und Firmware-OTA
   schließen sich gegenseitig aus (`online_update_in_progress()`), weil
   `WiFiClientSecure` + TLS-Session ≈ 40 KB intern kosten.
4. **Datei-Handles**: LittleFS ist mit `max_files = 10` gemountet. Der
   Server-Handler öffnet genau eine Datei pro Request und schließt sie vor
   `handle_return`; der Installer hält höchstens eine Zieldatei offen.
5. **Kein Verzeichnis-Cache im RAM.** Existenz/Größe werden pro Request über
   `LittleFS.open()`/`size()` ermittelt (externer Flash, Metadaten-Lesen ist
   billig); keine Liste installierter Dateien im Heap. Nur die installierte
   Versionskennung (≤ 16 B) darf nach dem Boot in `os`/einem statischen
   `char[16]` liegen – das ist der einzige erlaubte statische Zustand, weil
   `server_home()` bei jedem Seitenaufruf darauf prüft.
6. **PROGMEM/Flash statt RAM** für alles Konstante: MIME-Tabelle, Public Key
   (64 B), `/su`-Markup und eingebettetes `hasher.js` als `PROGMEM`-gz wie
   `ap_home_html_gz`; Ausgabe über `res.writeBodyData(__FlashStringHelper*)`.
7. **ESP8266 bleibt komplett außen vor**: alle neuen Dateien/Abschnitte unter
   `#if defined(ESP32) || defined(OSPI)`, damit dort weder Code noch RAM
   dazukommen.
8. **Messpflicht** (Teil der Abnahme in AP2 und AP3, Bench .151 in beiden
   Varianten, WiFi und W5500):
   - `free heap`/`min free heap` (`ESP.getMinFreeHeap()`) vor dem Feature,
     im Leerlauf mit lokaler UI, während des Seitenladens, während der
     Installation und nach Abschluss. Ziel: Leerlauf-Heap unverändert
     (± 1 KB), keine Verringerung des Minimums unter den heutigen Wert
     während einer Installation; `log_heap_snapshot()` dafür nutzen.
   - Stack-Hochwasser des Install-Tasks (`uxTaskGetStackHighWaterMark`) im
     Debug-Log, Task-Stack danach auf gemessenen Bedarf + 25 % setzen.
   - Keine Zunahme von `.bss`/`.data` im Linker-Map größer als die 16 B aus
     Punkt 5 (`firmware.map` vergleichen, `build.sh`-Hinweis ergänzen).
9. **OTF-Antwort**: `Content-Length` immer setzen und in 4-KB-Blöcken über
   `writeBodyData()` streamen; nie eine ganze Datei in `ether_buffer` oder
   einen `String` laden. `RESPONSE_BUFFER_SIZE`/`ETHER_BUFFER_SIZE` bleiben
   unverändert.

## 8. Umsetzungsstand 2026-11-01

Umgesetzt (Build 231, noch nicht committet):
- AP0: `ensureLogSpace()` / `logSpaceFloor()` in `sensors.cpp`, 6 MB Floor auf großen FS.
- AP1: `tools/mkuipack.py` (build/verify/list, ustar + gzip je Datei, Modul-Bundle, Minify via terser/cleancss, ECDSA-Signatur über openssl), `fw.sh`: `build_ui_package`, Manifest-Felder `ui_*`, Archivkopie `ui.tar`/`.sig`/`.json`, Katalogeintrag `ui_url`. Testpaket aus 2.4.0.230: 2,36 MB, 184 Dateien.
- AP2: `ui_local.cpp/.h` (Auslieferung `/ui/*` über `onMissingPage`, dynamische `versions.json`, Versionsauswahl, Migration der Quelle), `server_home()` lokal/extern/Fallback, `/jo` `uiv`/`uia`/`uist`, Optionen `IOPT_UI_SOURCE`/`IOPT_UI_FALLBACK` (`uisr`/`uifb`), `/su` neu mit eingebettetem `/js/hasher.js` (`html/hasher.js`, `compress_htmls.mjs` kann jetzt .js).
- AP3: Streaming-Tar-Installer mit SHA-256 + ECDSA (mbedtls / OpenSSL), Download-Task (`PSRAM_TASK_CREATE`, 12 KB) bzw. pthread auf OSPi (libcurl), `/ul`, `/uf` (ESP32, Port 8080), Installation am Ende der OTA-Phase 2, Auto-Check mit Backoff, GC auf zwei Versionen.
- AP4: App (`/srv/www/htdocs/ui`): `keyIndex` `uisr`:83/`uifb`:84, Auswahl + Fallback + Install-Button mit Fortschritt in den Systemeinstellungen (`options.js`). Übersetzungen fehlen noch (englische Texte).
- AP6: Endpoint-Doku, Plattform-Matrix, CHANGELOG 231.

Abweichung vom Plan (6/6.1): Das Paket wird **nicht** an das Firmware-Image
angehängt. Grund: Die C5-OTA ist zweiphasig und flasht beide Slots (Zigbee und
Matter) aus zwei Downloads; ein Anhang wäre doppelt übertragen worden, und
Geräte < 231 hätten die größere Datei abgelehnt. Stattdessen lädt die OTA am
Ende von Phase 2 das separate, signierte `ui.tar` nach (ein Updatevorgang aus
Nutzersicht, Dateinamen und Inhalte der Firmware-Binaries bleiben unverändert).

Offen:
- Messung am Bench (Ladezeit, Heap vor/nach, Stack-Hochwasser) und OSPi-Test.
- Übersetzungen der neuen App-Texte; Doku-Seite „UI-Quelle“ in `docs/docs` (de/en).
- Regel 7 aus 6.2 (ELF-Hash für Dev-Builds) nicht umgesetzt; Kopplung nur über die Versionsnummer.
- CE-Selbstbewertung (06_EN18031) um den zweiten signierten Update-Pfad ergänzen.

Messungen Bench .151 (Zigbee-Variante, Debug-Build, WiFi, 2026-11-01):
- Log-Floor: belegter Flash fiel nach dem ersten Sensorlog-Schreiben von 14,2 auf 5,4 MB (6-MB-Reserve greift).
- Installation des 2,4-MB-Testpakets von `http://192.168.0.50/ui-test/www/ui.tar`: 175 s (≈ 14 kB/s, Engpass ist das Schreiben vieler kleiner Dateien auf den externen Flash); Flash danach 8,0 MB belegt (+2,6 MB).
- Heap vor/nach Installation 38 139 / 38 047 B (Release-Build 230 lag bei ≈ 38 KB).
- Auslieferung: 17 Kern-Dateien, 486 KB gzip in 3,6 s sequentiell per curl; `js/modules/bundle.js` (232 KB) in 0,63 s.
- Headless Chromium (Login mit Passwort, Hauptseite gerendert): 11,4 s inkl. Passwort-Dialog und 3 s Wartezeit, 38 Requests, keine 404, keine Script-Fehler; extern nur Wetter-Requests.
- Installierte UI überlebt Reflash per USB und Neustart (`uiv` 2.4.0.231, `uia` 1).
- Korrektur aus dem Test: home.js schneidet `js/home.js` von seiner URL ab, der Loader liegt deshalb unter `/ui/js/home.js`.
- Offen: Installationsdauer verbessern (größerer Schreibpuffer/weniger Dateien), Matter-Variante und OSPi am Gerät testen.

