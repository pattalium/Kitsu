@echo off
setlocal

set "PROJECT=%~dp0.."
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo TEST_BLOCKED kitsu_message_read_contract: Visual C++ build tools not found
  exit /b 2
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%

set "OUT_DIR=%TEMP%\kitsu868-message-read-contract-test"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if errorlevel 1 exit /b %ERRORLEVEL%

pushd "%OUT_DIR%"
cl /nologo /std:c++17 /utf-8 /EHsc /W4 /WX ^
  "%PROJECT%\src\kitsu_message_read_contract.cpp" ^
  "%PROJECT%\tools\test_kitsu_message_read_contract.cpp" ^
  /Fe:"%OUT_DIR%\kitsu_message_read_contract_test.exe"
if errorlevel 1 (
  set "RESULT=%ERRORLEVEL%"
  popd
  exit /b %RESULT%
)

"%OUT_DIR%\kitsu_message_read_contract_test.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
