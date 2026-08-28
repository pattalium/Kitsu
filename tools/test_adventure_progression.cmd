@echo off
setlocal

set "PROJECT=%~dp0.."
set "OUT_DIR=%TEMP%\kitsu868-adventure-progression-test"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if errorlevel 1 exit /b %ERRORLEVEL%

set "GNUXX=%USERPROFILE%\.platformio\packages\toolchain-xtensa-esp32s3\bin\xtensa-esp32s3-elf-g++.exe"
if not exist "%GNUXX%" set "GNUXX=%USERPROFILE%\.platformio\packages\toolchain-xtensa-esp-elf\bin\xtensa-esp32s3-elf-g++.exe"
if not exist "%GNUXX%" (
  echo TEST_BLOCKED adventure_progression: PlatformIO GNU compiler not found
  exit /b 2
)

"%GNUXX%" -std=gnu++11 -Wall -Wextra -Werror ^
  -I"%PROJECT%\src" -fsyntax-only ^
  "%PROJECT%\src\adventure_progression.cpp" ^
  "%PROJECT%\tools\adventure_progression_host_test.cpp"
if errorlevel 1 exit /b %ERRORLEVEL%

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo TEST_BLOCKED adventure_progression: Visual C++ build tools not found
  exit /b 2
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%

pushd "%OUT_DIR%"
cl /nologo /std:c++14 /permissive- /EHsc /W4 /WX ^
  /I"%PROJECT%\src" ^
  "%PROJECT%\src\adventure_progression.cpp" ^
  "%PROJECT%\tools\adventure_progression_host_test.cpp" ^
  /Fe:"%OUT_DIR%\adventure_progression_test.exe"
if errorlevel 1 (
  set "RESULT=%ERRORLEVEL%"
  popd
  exit /b %RESULT%
)

"%OUT_DIR%\adventure_progression_test.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
