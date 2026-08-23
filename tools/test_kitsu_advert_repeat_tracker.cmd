@echo off
setlocal

set "PROJECT=%~dp0.."
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo TEST_BLOCKED kitsu_advert_repeat_tracker: Visual C++ build tools not found
  exit /b 2
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b %ERRORLEVEL%

set "OUT_DIR=%TEMP%\kitsu868-advert-repeat-tracker-test"
set "CRYPTO=%PROJECT%\.pio\libdeps\heltec_wifi_lora_32_V3_reflashable\Crypto"
set "MESHCORE=%PROJECT%\lib\MeshCore\src"
set "HOST_COMPAT=%PROJECT%\tools\host_compat"
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
if errorlevel 1 exit /b %ERRORLEVEL%

pushd "%OUT_DIR%"
cl /nologo /c /std:c++17 /EHsc /W0 /DHOST_BUILD ^
  /FI"%HOST_COMPAT%\crypto_compat.h" ^
  /I"%HOST_COMPAT%" /I"%CRYPTO%" /I"%CRYPTO%\utility" /I"%MESHCORE%" ^
  "%CRYPTO%\Crypto.cpp" "%CRYPTO%\Hash.cpp" "%CRYPTO%\SHA256.cpp" ^
  "%MESHCORE%\Packet.cpp"
if errorlevel 1 (
  set "RESULT=%ERRORLEVEL%"
  popd
  exit /b %RESULT%
)
cl /nologo /std:c++17 /EHsc /W4 /WX /wd4100 /wd4244 /wd4245 /DHOST_BUILD ^
  /FI"%HOST_COMPAT%\crypto_compat.h" ^
  /I"%HOST_COMPAT%" /I"%CRYPTO%" /I"%CRYPTO%\utility" /I"%MESHCORE%" ^
  "%PROJECT%\src\kitsu_repeat_wire.cpp" ^
  "%PROJECT%\src\kitsu_advert_repeat_tracker.cpp" ^
  "%PROJECT%\tools\test_kitsu_advert_repeat_tracker.cpp" ^
  Crypto.obj Hash.obj SHA256.obj Packet.obj ^
  /Fe:"%OUT_DIR%\kitsu_advert_repeat_tracker_test.exe"
if errorlevel 1 (
  set "RESULT=%ERRORLEVEL%"
  popd
  exit /b %RESULT%
)
"%OUT_DIR%\kitsu_advert_repeat_tracker_test.exe"
if errorlevel 1 (
  set "RESULT=%ERRORLEVEL%"
  popd
  exit /b %RESULT%
)
popd
echo Kitsu advert repeat tracker tests passed.
