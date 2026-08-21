@echo off
setlocal
set "PYTHON=%~dp0..\..\esptool-py310\Scripts\python.exe"
set "ESPSECURE=%~dp0..\..\esptool411-bootstrap\src\esptool-4.11.0\espsecure.py"
set "ESPEFUSE=%~dp0..\..\esptool411-bootstrap\src\esptool-4.11.0\espefuse.py"
"%PYTHON%" "%~dp0test_package_kitsu_production.py" --espsecure "%ESPSECURE%" --espefuse "%ESPEFUSE%"
exit /b %ERRORLEVEL%
