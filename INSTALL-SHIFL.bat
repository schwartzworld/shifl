@echo off
title SHIFL installer
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-shifl.ps1"
pause
