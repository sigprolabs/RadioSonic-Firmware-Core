@echo off
rem SPDX-License-Identifier: MIT
rem Copyright (c) 2026 SigPro Labs, LLC
rem Build and flash RadioSonic firmware. Takes the same options as flash.ps1,
rem e.g.  flash.cmd -Full   or   flash.cmd -Port COM7
rem
rem Windows refuses to run .ps1 scripts by default, and also refuses ones
rem extracted from a downloaded zip. Batch files are exempt, so this runs
rem flash.ps1 with the policy bypassed for this one run only. No system
rem setting changes.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash.ps1" %*
exit /b %ERRORLEVEL%
