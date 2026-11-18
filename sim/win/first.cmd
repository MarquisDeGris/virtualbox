@echo off
rem Part of the simulation, see ..\run.sh: runs once in the Windows guest when
rem the administrator is logged on for the first time after the installation.
rem Copies the report script to the disk and has it started now and at each
rem later start of Windows, by a scheduled task which needs nobody logged on.

echo WIN: FIRSTLOGON>COM1
mkdir C:\sim 2>nul
copy /y "%~dp0report.ps1" C:\sim\report.ps1 >nul
if errorlevel 1 echo WIN: FIRSTLOGON-FAILED copy>COM1

rem What Windows does when the power button is pressed is a setting, and not
rem every edition shuts down.  The simulation presses the button to see that
rem the press arrives, so here it is to shut Windows down.  What the setting
rem was is told first.
for /f "tokens=*" %%a in ('powercfg /qh SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION ^| findstr /c:"Power Setting Index"') do echo WIN: BUTTON-DEFAULT %%a>COM1
powercfg /setacvalueindex SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION 3 >C:\sim\powercfg.log 2>&1
powercfg /setdcvalueindex SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION 3 >>C:\sim\powercfg.log 2>&1
powercfg /setactive SCHEME_CURRENT >>C:\sim\powercfg.log 2>&1
if errorlevel 1 echo WIN: FIRSTLOGON-FAILED powercfg>COM1

schtasks /create /f /tn simreport /sc onstart /ru SYSTEM /rl HIGHEST /tr "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\sim\report.ps1" >C:\sim\schtasks.log 2>&1
if errorlevel 1 echo WIN: FIRSTLOGON-FAILED schtasks create>COM1
schtasks /run /tn simreport >>C:\sim\schtasks.log 2>&1
if errorlevel 1 echo WIN: FIRSTLOGON-FAILED schtasks run>COM1
exit /b 0
