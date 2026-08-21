@echo off
setlocal
py -3 "%~dp0test_kitsu_reflashable_profile.py"
exit /b %errorlevel%
