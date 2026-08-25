@echo off
setlocal

set "PROJECT=%~dp0.."
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo TEST_BLOCKED wild_creature_catalog: Visual C++ build tools not found
  exit /b 2
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
set "OUT_DIR=%TEMP%\kitsu868-wild-catalog-test"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
pushd "%OUT_DIR%"
cl /nologo /std:c++17 /EHsc /W4 /WX ^
  "%PROJECT%\src\signal_encounter.cpp" ^
  "%PROJECT%\src\wild_creature_catalog.cpp" ^
  "%PROJECT%\tools\test_wild_creature_catalog.cpp" ^
  /Fe:"%OUT_DIR%\wild_creature_catalog_test.exe"
if errorlevel 1 (
  set "RESULT=%ERRORLEVEL%"
  popd
  exit /b %RESULT%
)
"%OUT_DIR%\wild_creature_catalog_test.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
