@echo off
rem Builds the game's code library from your own MH4U update, USA v1.3.0 or Europe v1.1.0 (see build-code-library.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-code-library.ps1" %*
