@echo off
setlocal

set "PROJECT=%~dp0.."
set "OUT_DIR=%TEMP%\kitsu868-companion-dialogue-test"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if errorlevel 1 exit /b %ERRORLEVEL%

rem Compile the firmware translation unit with its actual GCC family and the
rem project's required language level before running the host assertions.
set "XTENSA_CXX=%USERPROFILE%\.platformio\packages\toolchain-xtensa-esp32s3\bin\xtensa-esp32s3-elf-g++.exe"
if not exist "%XTENSA_CXX%" (
  echo TEST_BLOCKED companion_dialogue: Xtensa C++ compiler not found
  exit /b 2
)
"%XTENSA_CXX%" -std=gnu++11 -Wall -Wextra -Werror ^
  -I"%PROJECT%\src" -c "%PROJECT%\src\companion_dialogue.cpp" ^
  -o "%OUT_DIR%\companion_dialogue-gnu11.o"
if errorlevel 1 exit /b %ERRORLEVEL%

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo TEST_BLOCKED companion_dialogue: Visual C++ build tools not found
  exit /b 2
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%

pushd "%OUT_DIR%"
cl /nologo /std:c++14 /EHsc /W4 /WX ^
  "%PROJECT%\src\companion_dialogue.cpp" ^
  "%PROJECT%\tools\companion_dialogue_host_test.cpp" ^
  /Fe:"%OUT_DIR%\companion_dialogue_test.exe"
if errorlevel 1 (
  set "RESULT=%ERRORLEVEL%"
  popd
  exit /b %RESULT%
)

"%OUT_DIR%\companion_dialogue_test.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
