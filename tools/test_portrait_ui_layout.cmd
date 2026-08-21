@echo off
setlocal

set "PROJECT=%~dp0.."
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo TEST_BLOCKED portrait_ui_layout: Visual C++ build tools not found
  exit /b 2
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%

set "OUT_DIR=%TEMP%\kitsu868-portrait-ui-test"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if errorlevel 1 exit /b %ERRORLEVEL%

pushd "%OUT_DIR%"
cl /nologo /std:c++17 /EHsc /W4 /WX ^
  "%PROJECT%\tests\portrait_ui_layout_host.cpp" ^
  /Fe:"%OUT_DIR%\portrait_ui_layout_test.exe"
if errorlevel 1 (
  set "RESULT=%ERRORLEVEL%"
  popd
  exit /b %RESULT%
)

"%OUT_DIR%\portrait_ui_layout_test.exe"
if errorlevel 1 (
  set "RESULT=%ERRORLEVEL%"
  popd
  exit /b %RESULT%
)
echo TEST_PASS portrait_ui_layout all_screens all_phone_states dynamic_bounds
set "RESULT=0"
popd
exit /b %RESULT%
