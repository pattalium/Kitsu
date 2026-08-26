@echo off
setlocal

if "%~1"=="" (
    echo Usage: %~nx0 OUTPUT_DIRECTORY [FIRMWARE_VERSION]
    exit /b 2
)

set "PROJECT_ROOT=%~dp0.."
rem Use the pinned local PlatformIO Python runtime and the exact esptool 4.11
rem package installed beside its offline PlatformIO package cache.  Both paths
rem are resolved from this checkout, so the runner does not depend on a global
rem Python installation or a stale user-profile tool directory.
for %%I in ("%~dp0..\..\platformio-core-runtime\Scripts\python.exe") do set "PYTHON=%%~fI"
for %%I in ("%~dp0..\..\..\private\tooling\platformio-core\packages\tool-esptoolpy\esptool.py") do set "ESPTOOL=%%~fI"
rem The packager verifies that this exact generated header is recorded in the
rem PlatformIO build signature before trusting its rollback/security settings.
for %%I in ("%~dp0..\..\..\private\tooling\platformio-core\packages\framework-arduinoespressif32\tools\sdk\esp32s3\qio_qspi\include\sdkconfig.h") do set "SDKCONFIG=%%~fI"
set "BUILD_DIR=%PROJECT_ROOT%\.pio\build\heltec_wifi_lora_32_V3_reflashable"
set "FIRMWARE_VERSION=%~2"
if "%FIRMWARE_VERSION%"=="" set "FIRMWARE_VERSION=0.17.4"

"%PYTHON%" "%~dp0package_kitsu_reflashable.py" ^
    --project-root "%PROJECT_ROOT%" ^
    --build-dir "%BUILD_DIR%" ^
    --output-dir "%~1" ^
    --esptool "%ESPTOOL%" ^
    --sdkconfig "%SDKCONFIG%" ^
    --firmware-version "%FIRMWARE_VERSION%"
exit /b %ERRORLEVEL%
