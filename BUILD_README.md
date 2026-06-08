# Eigener RTTR-Quellbaum mit AdvancedAI – Bauen & Nutzen

Dieser Baum (`C:\RTTR\s25client`) ist ein vollständiger Klon von
[`Return-To-The-Roots/s25client`](https://github.com/Return-To-The-Roots/s25client)
(inkl. aller Submodule), in den unsere KI **AdvancedAIPlayer** integriert ist.

## Toolchain & Abhängigkeiten

- **MSYS2 / UCRT64** unter `C:\msys64` (bereits vorhanden) – GCC 16, CMake, Ninja.
- Abhängigkeiten als UCRT64-Pakete (bereits installiert): **Boost, SDL2,
  SDL2_mixer, BZip2, Lua, libsamplerate, miniupnpc, gettext, curl**.
- Diese sind ABI-kompatibel zur installierten WinLibs-UCRT-Toolchain.

## Bauen

Einfach die Batch ausführen:

```bat
C:\RTTR\s25client\build.bat
```

Sie stellt fehlende Pakete per `pacman` sicher, konfiguriert mit CMake (Ninja,
Release) und baut das Ziel `s25client`.

**Manuell** (in der MSYS2-UCRT64-Shell) entspricht das:

```bash
cd /c/RTTR/s25client
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build -j --target s25client
```

Ergebnis: **`C:\RTTR\s25client\build\s25client.exe`** (v20260606, GCC 16.1, Win64 — verifiziert lauffähig)

Inkrementell nach Code-Änderungen: nur den letzten Befehl erneut ausführen.

## Unsere KI (AdvancedAI) ein-/ausschalten

Die Integration ist **opt-in** und ändert das Standardverhalten nicht:

- Quellen: `libs/s25main/ai/advai/` (eingebunden via `AddDirectory(ai/advai)`
  in `libs/s25main/CMakeLists.txt`).
- Schalter in `libs/s25main/factories/AIFactory.cpp`: Ist die Umgebungsvariable
  **`RTTR_USE_ADVAI`** gesetzt, ersetzt `AdvancedAIPlayer` die Standard-KI
  (`AIPlayerJH`) für alle „Default"-KI-Spieler. Ohne die Variable bleibt alles
  unverändert.

```bat
set RTTR_USE_ADVAI=1
```

Tuning-Parameter der KI optional über `RTTR_AI_PARAMS` (Pfad zu einer
Parameterdatei, siehe `rttr-ai/AIParams.h` bzw. `harness/`).

## Ausführen des selbstgebauten Clients

Die `s25client.exe` braucht zur Laufzeit:
1. die **UCRT64-DLLs** (Boost/SDL2/Lua/GCC-Runtime) – am einfachsten, indem
   `C:\msys64\ucrt64\bin` im `PATH` steht;
2. die **Spieldaten** (RTTR-Daten + originale S2-Daten).

Empfohlener Weg – Installations-Layout erzeugen und Daten danebenlegen:

```bash
cmake --install build --prefix /c/RTTR/run
```

Anschließend in `C:\RTTR\run` die originalen **S2-Daten** bereitstellen (wie in
der lauffähigen Version unter `...\Siedler\s25rttr_20260209\` – Ordner `DATA`,
`GFX` usw.), die UCRT64-DLLs daneben kopieren bzw. `C:\msys64\ucrt64\bin` in den
`PATH` aufnehmen, dann starten:

```bat
set RTTR_USE_ADVAI=1
C:\RTTR\run\bin\s25client.exe
```

> Hinweis: Die DLLs der vorhandenen lauffähigen Version stammen aus einer
> **anderen** MinGW-Variante – nicht mit unserem UCRT64-Build mischen. Entweder
> komplett UCRT64-DLLs verwenden oder den Client aus der MSYS2-UCRT64-Shell
> heraus starten (dort sind die DLLs bereits im `PATH`).

### Schnellster Weg (ohne install)

Der `build`-Ordner ist bereits eine lauffähige Umgebung (enthält `driver/`,
`RTTR/` und die nötigen DLLs). Es fehlen nur die originalen **S2-Daten**:

```bat
REM einmalig die S2-Daten aus der lauffaehigen Version uebernehmen:
xcopy /E /I /Y "C:\Users\jkost\OneDrive\Desktop\Siedler\s25rttr_20260209\DATA" "C:\RTTR\s25client\build\S2\DATA"
xcopy /E /I /Y "C:\Users\jkost\OneDrive\Desktop\Siedler\s25rttr_20260209\GFX"  "C:\RTTR\s25client\build\S2\GFX"

REM dann mit unserer KI starten:
run-advai.bat
```

(Falls der Client einen anderen S2-Pfad erwartet, beim ersten Start im Menü die
S2-Installation auf `...\s25rttr_20260209` zeigen lassen.)

### KI-Partie direkt über die Kommandozeile

Der Client kann Karte + KI-Spieler per CLI laden (ideal für schnelle Tests):

```bat
run-advai.bat --map "RTTR\MAPS\<karte>.swd" --ai AIJH --ai AIJH
```

Mit gesetztem `RTTR_USE_ADVAI` (steckt in `run-advai.bat`) werden die „Default"-
KI-Spieler durch unsere **AdvancedAI** ersetzt – so spielt AdvancedAI direkt auf
einer echten Karte.

## Headless Self-Play-Harness (optional, für KI-Tuning)

Der `MatchRunner` (`rttr-ai/harness/`) braucht die Test-Fixtures und damit einen
Build **mit** `-DBUILD_TESTING=ON` (zieht Boost.Test/turtle nach). Integration:
`MatchRunner.cpp` als eigenes Target gegen `s25Main` + Test-Helfer linken –
Details in `rttr-ai/harness/README.md`.
