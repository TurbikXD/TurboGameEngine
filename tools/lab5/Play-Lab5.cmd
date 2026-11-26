@echo off
pushd "%~dp0" || exit /b 1
app.exe --lab5 "game\Lab5Beacon.tgeproj"
set "TGE_LAB5_EXIT=%ERRORLEVEL%"
popd
exit /b %TGE_LAB5_EXIT%
