@echo off
rem vm - run a command in / open a shell on the supervisor's first running VM.
rem Put this directory on PATH (or copy this file somewhere that already is).
python "%~dp0vm.py" %*
