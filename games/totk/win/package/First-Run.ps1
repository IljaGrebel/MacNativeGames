<#
.SYNOPSIS
  First run of the Windows port's M0 (boot without graphics) on Windows, from the prebuilt package.

.DESCRIPTION
  Prepares your own Tears of the Kingdom 1.0.0 dump with the bundled portable Python (ExeFS, RomFS, fonts, module
  manifests; the upstream extraction scripts), then boots it with totk-win.exe. The first boot recompiles the game's
  code for this CPU (10-40 minutes); later boots load it from the cache. Nothing is installed: everything stays in this
  folder. The log is first-run.log here: send it back.

  Without -Game / -Keys it asks for the files in a dialog.

.EXAMPLE
  .\First-Run.ps1 -Game D:\Switch\TotK.xci -Keys D:\Switch\prod.keys
#>
[CmdletBinding()]
param(
    [string]$Game,
    [string]$Keys,
    [string]$Firmware,          # unpacked firmware NCA folder: the shared fonts when the game is an NSP
    [switch]$SkipRomfs,         # modules only: the game stops where it first reads RomFS
    [int]$TimeoutMinutes = 90
)
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Totk = Join-Path $Root 'prep\games\totk'
$Python = Join-Path $Root 'python\python.exe'
$Exe = Join-Path $Root 'bin\totk-win.exe'
$Log = Join-Path $Root 'first-run.log'

function Step($text) { Write-Host "`n== $text" -ForegroundColor Cyan }

function Pick-File($title, $filter) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = $title
    $dialog.Filter = $filter
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw "No file chosen: $title" }
    $dialog.FileName
}

# Files from a downloaded zip carry the internet zone mark; Windows would ask about every program in the folder.
Get-ChildItem -LiteralPath $Root -Recurse -File | Unblock-File

Step 'checks'
if (-not [Environment]::Is64BitOperatingSystem) { throw 'A 64-bit Windows is needed.' }
foreach ($p in $Python, $Exe) { if (-not (Test-Path -LiteralPath $p)) { throw "Missing ${p}: unpack the whole package." } }
$drive = (Get-Item -LiteralPath $Root).PSDrive
$freeGB = [math]::Floor($drive.Free / 1GB)
Write-Host "free space on ${drive}: $freeGB GB"
if (-not $SkipRomfs -and -not (Test-Path (Join-Path $Totk 'extracted\v0\romfs\index.json')) -and $freeGB -lt 40) {
    throw "About 40 GB free are needed on $drive (the RomFS alone is about 17 GB). Move this folder to a larger drive."
}
if (-not $Game) { $Game = Pick-File 'Tears of the Kingdom 1.0.0 (XCI or NSP, base game, no update)' 'Switch game (*.xci;*.nsp)|*.xci;*.nsp' }
if (-not $Keys) { $Keys = Pick-File 'prod.keys from your console' 'prod.keys|prod.keys;*.keys|All files|*.*' }
foreach ($p in $Game, $Keys) { if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "Not found: $p" } }

Step 'your dump: ExeFS, RomFS, fonts, module manifests'
$prepArgs = @('-I', (Join-Path $Totk 'win\tools\prepare_game.py'), $Game, '--keys', $Keys)
if ($Firmware) { $prepArgs += @('--firmware', $Firmware) }
if ($SkipRomfs) { $prepArgs += '--skip-romfs' }
$env:PYTHONUTF8 = '1'; $env:PYTHONIOENCODING = 'utf-8'   # the scripts print paths and names in any language
& $Python @prepArgs
if ($LASTEXITCODE) { throw "Preparing the dump failed (exit code $LASTEXITCODE): see the messages above." }

Step "boot (log: $Log)"
$nxm = Join-Path $Totk 'extracted\v0\nxm'
$env:TOTK_ROMFS = Join-Path $Totk 'extracted\v0\romfs'
$env:TOTK_PROFILE_DIR = Join-Path $Root 'data\profile'
$env:TOTK_WIN_CONFIG_DIR = Join-Path $Totk 'build'
$env:TOTK_SHARED_FONTS = Join-Path $Totk 'extracted\firmware\shared-fonts'
$env:TOTK_WIN_RECOMP_VERBOSE = '1'
New-Item -ItemType Directory -Force -Path $env:TOTK_PROFILE_DIR | Out-Null
$cache = Join-Path $Root 'data\code-cache'
Write-Host 'The first start recompiles the game''s code for this CPU (can take 10-40 minutes); later starts load it from the cache.'

# cmd does the redirection: PowerShell 5.1 would turn every stderr line into an error record.
$cmdline = "`"`"$Exe`" --cache `"$cache`" `"$nxm\main.nxm`" `"$nxm\subsdk0.nxm`" `"$nxm\sdk.nxm`" > `"$Log`" 2>&1`""
$proc = Start-Process -FilePath $env:ComSpec -ArgumentList '/d', '/s', '/c', $cmdline -WorkingDirectory $Totk `
    -NoNewWindow -PassThru
$null = $proc.Handle   # without the handle taken now, Windows PowerShell reports no exit code afterwards
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
$shown = 0L
$pattern = 'recompil|units|\[totk-win\]|ENTER|FATAL'
while ($true) {
    $done = $proc.WaitForExit(2000)
    if (Test-Path -LiteralPath $Log) {
        $stream = [System.IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
        try {
            if ($stream.Length -gt $shown) {
                $stream.Seek($shown, 'Begin') | Out-Null
                $reader = New-Object System.IO.StreamReader($stream)
                $text = $reader.ReadToEnd()
                $cut = $text.LastIndexOf("`n")
                if ($cut -ge 0 -or $done) {
                    if (-not $done) { $text = $text.Substring(0, $cut + 1) }
                    $shown += [System.Text.Encoding]::UTF8.GetByteCount($text)
                    $text -split "`r?`n" | Where-Object { $_ -match $pattern } | ForEach-Object { Write-Host $_ }
                }
            }
        } finally { $stream.Dispose() }
    }
    if ($done) { break }
    if ((Get-Date) -gt $deadline) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        Get-CimInstance Win32_Process -Filter "Name = 'totk-win.exe'" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        Write-Host "stopped after $TimeoutMinutes minutes" -ForegroundColor Yellow
        break
    }
}
$rc = if ($proc.HasExited) { $proc.ExitCode } else { 124 }
Add-Content -LiteralPath $Log -Value "exit code $rc"

Step 'result'
$lines = Get-Content -LiteralPath $Log
$lines | Select-String -Pattern '\[totk-win\]|ENTER|RETURN|nninit|nnMain|\[nvn\] #(1|2|3|4|5) |FATAL|unimplemented|unresolved' |
    Select-Object -First 60 | ForEach-Object { Write-Host $_.Line }
$nvn = ($lines | Select-String -Pattern '^\[nvn\]').Count
Write-Host ''
Write-Host "NVN calls logged: $nvn   exit code: $rc"
Write-Host "Full log: $Log  (send this file back)" -ForegroundColor Green
