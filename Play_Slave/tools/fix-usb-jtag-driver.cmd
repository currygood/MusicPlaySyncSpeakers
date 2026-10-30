@echo off
chcp 437 >nul
setlocal
title ESP32-S3 USB-JTAG driver fix

net session >nul 2>&1
if errorlevel 1 (
  echo.
  echo [X] Administrator rights required.
  echo     Right-click this file - "Run as administrator".
  echo.
  pause
  exit /b 1
)

set "INF=F:\Espressif\tools\idf-driver\idf-driver-esp32-usb-jtag-2021-07-15\USB_JTAG_debug_unit.inf"
if not exist "%INF%" (
  echo.
  echo [X] INF not found: %INF%
  echo.
  pause
  exit /b 1
)

echo ============================================================
echo  Step 1/4 : add official WinUSB driver package
echo ============================================================
pnputil /add-driver "%INF%" /install

echo.
echo ============================================================
echo  Step 2/4 : UNPLUG the native USB cable (port labelled USB)
echo             then press any key
echo ============================================================
pause >nul

echo.
echo ============================================================
echo  Step 3/4 : remove the wrong usbser binding (oem173.inf)
echo ============================================================
pnputil /delete-driver oem173.inf /uninstall /force

echo.
echo ============================================================
echo  Step 4/4 : plug the native USB cable back in,
echo             then press any key to rescan
echo ============================================================
pause >nul
pnputil /scan-devices

echo.
echo ------------------------------------------------------------
echo  Expected: Interface 2 = class USBDevice, name "USB JTAG debug unit"
echo  Current state:
echo ------------------------------------------------------------
powershell -NoProfile -Command "Get-PnpDevice | Where-Object { $_.InstanceId -match 'VID_303A' } | Select-Object Status,Class,FriendlyName,InstanceId | Format-Table -AutoSize"

echo.
echo If Interface 2 is still class Ports / COMxx, do it manually:
echo   Device Manager - Ports - USB JTAG/serial debug unit (Interface 2)
echo   - Update driver - Browse - pick from a list - Have Disk - %INF%
echo.
pause
