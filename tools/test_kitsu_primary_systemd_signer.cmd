@echo off
setlocal
py -3 "%~dp0test_kitsu_primary_systemd_signer.py"
exit /b %errorlevel%
