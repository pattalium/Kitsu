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
set "BUILD_DIR=%PROJECT_ROOT%\.pio\build\heltec_wifi_lora_32_V3_reflashable"
set "FIRMWARE_VERSION=%~2"
if "%FIRMWARE_VERSION%"=="" set "FIRMWARE_VERSION=0.11.0"

"%PYTHON%" "%~dp0package_kitsu_reflashable.py" ^
    --project-root "%PROJECT_ROOT%" ^
    --build-dir "%BUILD_DIR%" ^
    --output-dir "%~1" ^
    --esptool "%ESPTOOL%" ^
    --firmware-version "%FIRMWARE_VERSION%"
exit /b %ERRORLEVEL%
