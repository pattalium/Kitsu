@echo off
setlocal

set "PROJECT=%~dp0.."
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo TEST_BLOCKED kitsu_endpoint_rx_policy: Visual C++ build tools not found
  exit /b 2
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%

set "OUT_DIR=%TEMP%\kitsu868-endpoint-rx-policy-test"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if errorlevel 1 exit /b %ERRORLEVEL%

cl /nologo /std:c++17 /EHsc /W4 /WX ^
  "%PROJECT%\tools\test_kitsu_endpoint_rx_policy.cpp" ^
  /Fe:"%OUT_DIR%\kitsu_endpoint_rx_policy_test.exe"
if errorlevel 1 exit /b %ERRORLEVEL%
"%OUT_DIR%\kitsu_endpoint_rx_policy_test.exe"
if errorlevel 1 exit /b %ERRORLEVEL%
echo Kitsu endpoint RX burst policy tests passed.
