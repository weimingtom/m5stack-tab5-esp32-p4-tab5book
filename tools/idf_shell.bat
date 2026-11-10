@echo off
rem Helper: open an ESP-IDF shell in this project.
set IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.6
set IDF_TOOLS_PATH=D:\Espressif
set PATH=D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;D:\Espressif\tools\idf-git\2.44.0\cmd;%PATH%
call "%IDF_PATH%\export.bat"
cd /d "%~dp0.."
cmd /k
