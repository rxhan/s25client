@ECHO OFF
REM ===========================================================================
REM  Startet ein BEOBACHTBARES KI-Duell:  AdvancedAI (Slot 0)  vs  AIPlayerJH (Slot 1).
REM  Der Mensch ist nur Zuschauer (AI-Battle-Modus von s25client).
REM
REM  WICHTIG: Hier wird RTTR_USE_ADVAI bewusst NICHT gesetzt – sonst wuerde auch
REM  der "aijh"-Slot zur AdvancedAI. Die Slot-Auswahl macht "--ai advai --ai aijh".
REM
REM  Karte: optionales 1. Argument; Standard = ALASKA (2 Spieler). Eigene Karte:
REM     run-adv-vs-jh.bat "C:\Pfad\zur\karte.swd"
REM ===========================================================================
SETLOCAL
SET "PATH=C:\msys64\ucrt64\bin;%PATH%"

REM Optional: getunte Holz-/KI-Parameter fuer die AdvancedAI laden
REM SET "RTTR_AI_PARAMS=C:\RTTR\best.aiparams"

SET "MAP=%~1"
IF "%MAP%"=="" SET "MAP=C:\RTTR\s25client\data\RTTR\MAPS\OTHER\ALASKA.SWD"

CD /D "%~dp0build"
s25client.exe --map "%MAP%" --ai advai --ai aijh
ENDLOCAL
