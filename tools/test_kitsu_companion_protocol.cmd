@echo off
setlocal

set "PROJECT=%~dp0.."
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo TEST_BLOCKED kitsu_companion_protocol: Visual C++ build tools not found
  exit /b 2
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%

set "OUT_DIR=%TEMP%\kitsu868-companion-protocol-test"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if errorlevel 1 exit /b %ERRORLEVEL%

pushd "%OUT_DIR%"
cl /nologo /std:c++17 /EHsc /W4 /WX ^
  "%PROJECT%\src\kitsu_companion_protocol.cpp" ^
  "%PROJECT%\tools\test_kitsu_companion_protocol.cpp" ^
  bcrypt.lib ^
  /Fe:"%OUT_DIR%\kitsu_companion_protocol_test.exe"
if errorlevel 1 (
  popd
  exit /b 1
)
"%OUT_DIR%\kitsu_companion_protocol_test.exe"
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" (
  popd
  exit /b %RESULT%
)
popd
echo Kitsu companion protocol tests passed.
