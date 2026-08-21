@echo off
setlocal
set "PYTHON=%~dp0..\..\esptool-py310\Scripts\python.exe"
set "ESPSECURE=%~dp0..\..\esptool411-bootstrap\src\esptool-4.11.0\espsecure.py"
"%PYTHON%" "%~dp0test_owner_custody_sign_cms.py" --espsecure "%ESPSECURE%"
exit /b %errorlevel%
