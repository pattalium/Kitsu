@echo off
setlocal
py -3 "%~dp0test_package_kitsu_reflashable.py"
exit /b %ERRORLEVEL%
