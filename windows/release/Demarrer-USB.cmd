@echo off
cd /d %~dp0
adb shell am start -n com.screenlink36.app/.MainActivity
start "" 36Screenlink.exe --usb --tray
