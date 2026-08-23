@echo off
setlocal

if "%~1"=="" (
    echo Usage: %~nx0 OUTPUT_DIRECTORY [FIRMWARE_VERSION]
    exit /b 2
)

set "PROJECT_ROOT=%~dp0.."
rem Use the independently installed, runnable esptool 4.11 environment for both
rem the packager and its mandatory image_info validation.  Do not fall back to
rem a source checkout whose Python dependencies may be incomplete.
set "PYTHON=%~dp0..\..\esptool411-runtime\Scripts\python.exe"
set "ESPTOOL=%~dp0..\..\esptool411-runtime\Scripts\esptool.exe"
rem The packager verifies that this exact generated header is recorded in the
rem PlatformIO build signature before trusting its rollback/security settings.
set "SDKCONFIG=%USERPROFILE%\.platformio\packages\framework-arduinoespressif32\tools\sdk\esp32s3\qio_qspi\include\sdkconfig.h"
set "BUILD_DIR=%PROJECT_ROOT%\.pio\build\heltec_wifi_lora_32_V3_reflashable"
set "FIRMWARE_VERSION=%~2"
if "%FIRMWARE_VERSION%"=="" set "FIRMWARE_VERSION=0.16.5"

"%PYTHON%" "%~dp0package_kitsu_reflashable.py" ^
    --project-root "%PROJECT_ROOT%" ^
    --build-dir "%BUILD_DIR%" ^
    --output-dir "%~1" ^
    --esptool "%ESPTOOL%" ^
    --sdkconfig "%SDKCONFIG%" ^
    --firmware-version "%FIRMWARE_VERSION%"
exit /b %ERRORLEVEL%
