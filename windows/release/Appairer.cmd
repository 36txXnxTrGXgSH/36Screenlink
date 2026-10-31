@echo off
cd /d %~dp0
adb shell am start -n com.screenlink36.app/.MainActivity
36Screenlink.exe --pair
pause
