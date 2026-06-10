@ECHO OFF
REM ===========================================================================
REM  Startet den selbstgebauten s25client MIT unserer KI (AdvancedAI).
REM
REM  - Setzt die UCRT64-DLLs auf den PATH (Boost/SDL2/Lua/GCC-Runtime).
REM  - Aktiviert unsere KI via RTTR_USE_ADVAI: ersetzt ALLE "Default"-KIs (JH)
REM    global durch die AdvancedAI (Adv vs Adv).
REM
REM  Fuer GEMISCHTE Partien (AdvancedAI GEGEN JH) NICHT diese Datei nehmen:
REM   * In der Lobby den Slot durchklicken bis "(advanced)" – pro Slot waehlbar
REM     (Default(hard) -> advanced -> Dummy). Anderer Slot bleibt auf "(hard)" = JH.
REM   * Oder fertiges Zuschauer-Duell:  run-adv-vs-jh.bat  (nutzt --ai advai --ai aijh).
REM
REM  Beim ersten Start nach den originalen Settlers-II-Daten fragen lassen bzw.
REM  die S2-Daten (Ordner DATA, GFX ...) aus der lauffaehigen Version kopieren,
REM  z.B. aus  ...\Siedler\s25rttr_20260209\   (siehe BUILD_README.md).
REM ===========================================================================
SETLOCAL
SET "PATH=C:\msys64\ucrt64\bin;%PATH%"
SET "RTTR_USE_ADVAI=1"

REM Optional: eigene KI-Tuning-Parameter aus Datei laden
REM SET "RTTR_AI_PARAMS=C:\RTTR\best.aiparams"

CD /D "%~dp0build"
s25client.exe %*
ENDLOCAL
