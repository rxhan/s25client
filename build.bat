@ECHO OFF
REM ===========================================================================
REM  RTTR (s25client) inkl. AdvancedAI bauen - MinGW/UCRT64 via MSYS2
REM
REM  Voraussetzung: MSYS2 unter C:\msys64 (mit UCRT64-Toolchain + Abhaengig-
REM  keiten). Fehlende Pakete werden via pacman nachinstalliert.
REM
REM  Ergebnis: build\s25client.exe
REM  Mit unserer KI spielen: Umgebungsvariable RTTR_USE_ADVAI setzen (siehe unten).
REM ===========================================================================
SETLOCAL
SET "MSYS=C:\msys64"
SET "BASH=%MSYS%\usr\bin\bash.exe"
SET "SRC=/c/RTTR/s25client"

IF NOT EXIST "%BASH%" (
  ECHO FEHLER: MSYS2 nicht unter %MSYS% gefunden.
  ECHO Bitte MSYS2 installieren: https://www.msys2.org/  ^(oder: winget install MSYS2.MSYS2^)
  EXIT /B 1
)

ECHO === [1/3] Abhaengigkeiten sicherstellen ^(pacman, idempotent^) ===
"%BASH%" -lc "pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-boost mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-SDL2_mixer mingw-w64-ucrt-x86_64-bzip2 mingw-w64-ucrt-x86_64-gettext mingw-w64-ucrt-x86_64-libsamplerate mingw-w64-ucrt-x86_64-miniupnpc mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-lua"
IF ERRORLEVEL 1 ( ECHO FEHLER bei der Paketinstallation. & EXIT /B 1 )

ECHO === [2/3] CMake konfigurieren ^(Ninja, Release^) ===
REM RTTR_ENABLE_WERROR=OFF: noetig, da der sehr neue GCC 16 neue Warnungen
REM erzeugt, die der Code (noch) nicht erfuellt; sonst brechen sie den Build ab.
REM LUA_*: kaguya (Lua-Binding) unterstuetzt kein Lua 5.4/5.5 -> auf das
REM installierte Lua 5.3 (Paket lua53) zwingen.
"%BASH%" -lc "export MSYSTEM=UCRT64; source /etc/profile; cd %SRC% && cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DRTTR_ENABLE_OPTIMIZATIONS=ON -DRTTR_ENABLE_WERROR=OFF -DBUILD_TESTING=OFF -DLUA_INCLUDE_DIR=C:/msys64/ucrt64/include/lua5.3 -DLUA_LIBRARY=C:/msys64/ucrt64/lib/liblua5.3.dll.a -DLUA_LIBRARIES=C:/msys64/ucrt64/lib/liblua5.3.dll.a"
IF ERRORLEVEL 1 ( ECHO FEHLER bei der CMake-Konfiguration. & EXIT /B 1 )

ECHO === [3/3] Bauen ^(s25client^) ===
"%BASH%" -lc "export MSYSTEM=UCRT64; source /etc/profile; cd %SRC% && cmake --build build -j --target s25client"
IF ERRORLEVEL 1 ( ECHO FEHLER beim Bauen. & EXIT /B 1 )

ECHO.
ECHO ============================================================
ECHO  FERTIG. Binary: C:\RTTR\s25client\build\s25client.exe
ECHO.
ECHO  Schnelltest:   build\s25client.exe --version
ECHO  Spielen mit AdvancedAI:  run-advai.bat   ^(siehe BUILD_README.md^)
ECHO ============================================================
ENDLOCAL
