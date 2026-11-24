@echo off
REM Flash the from-source Muse firmware (multi-Wi-Fi, 8 saved networks) to AIPI Lite
REM Usage: flash.bat COMx   (example: flash.bat COM3)
REM Find the COM port in Device Manager ^> Ports (COM ^& LPT) ^> "USB JTAG/serial debug unit (COMx)"
REM Needs: Python 3.12 + "pip install esptool". Box plugged in with the USB-C DATA cable.
REM NEVER run erase-flash: it wipes pairing, the SDK token, and saved Wi-Fi.
if "%1"=="" (
  echo Usage: flash.bat COMx  (example: flash.bat COM3)
  echo Find the port in Device Manager ^> Ports (COM ^& LPT) ^> "USB JTAG/serial debug unit (COMx)"
  pause
  exit /b 1
)
python -m esptool --chip esp32s3 -p %1 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size keep --flash-freq 80m 0x0 bootloader.bin 0x10000 partition-table.bin 0x1d000 ota_data_initial.bin 0x20000 muse-gadget.bin
echo.
echo Done. Unplug the box, and it should boot, rejoin the park Wi-Fi, and show the face.
pause
