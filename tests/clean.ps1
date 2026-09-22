# tests\clean.ps1 — remove transient artifacts.  No caches to manage.

param([switch]$Full)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

Get-ChildItem -Path . -Filter "_vayu_*" -File -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host ("rm " + $_.Name); Remove-Item $_.FullName -Force }

foreach ($f in @(
    "vayu_self.ssa","vayu_self.s","vayu_self2.ssa","vayu_rt.c",
    "vayu_qbe.log","vayu_gcc.log","vayu_sc.log",
    "vcode_self.ssa","vcode_self.s","vcode_self2.ssa","vcode_rt.c",
    "vcode_qbe.log","vcode_gcc.log","vcode_sc.log",
    ".vayu_rt_cache.c",".fixpoint_vayu.cache",".fixpoint_vcode.cache",
    ".fixpoint.cmd")) {
    if (Test-Path $f) { Write-Host ("rm " + $f); Remove-Item $f -Force -ErrorAction SilentlyContinue }
}

foreach ($f in @(
    "tests\.fixpoint_vcode.cache","tests\.fixpoint_vayu.cache",
    "tests\.vayu_rt_cache.c","tests\.fixpoint.cmd",
    "tests\hello_fmt.vyu","tests\hello_fmt2.vyu")) {
    if (Test-Path $f) { Write-Host ("rm " + $f); Remove-Item $f -Force -ErrorAction SilentlyContinue }
}

if ($Full) {
    foreach ($f in @("vayu.exe","vcode.exe","nva.exe")) {
        if (Test-Path $f) { Write-Host ("rm " + $f); Remove-Item $f -Force }
    }
    Write-Host "(full clean)"
}

Write-Host ""
Write-Host "Clean."