@echo off
setlocal
where py >nul 2>nul
if errorlevel 1 goto python
py -3 "%~dp0ram-boot.py" %*
goto done
:python
where python >nul 2>nul
if errorlevel 1 (
  echo Install Python 3 from https://www.python.org/downloads/windows/ then retry.
  exit /b 1
)
python "%~dp0ram-boot.py" %*
:done
set "status=%errorlevel%"
exit /b %status%
