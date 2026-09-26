# tests/clean.ps1
# Removes transient native-compiler artifacts.  Keeps the built tools
# (nva.exe, vayu.exe, vcode.exe, vlex.exe, vparse.exe) and the CMake
# build tree.  Use -Full to wipe the tools too.

param(
    [switch]$Full
)

$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $root

$kept = @('nva.exe', 'vayu.exe', 'vcode.exe', 'vlex.exe', 'vparse.exe')

# Transient artifacts anywhere in the repo root (not recursive).
Get-ChildItem -File -Filter '_vayu_*' | ForEach-Object {
    Write-Host ("rm " + $_.Name)
    Remove-Item $_.FullName -Force
}
Get-ChildItem -File -Filter '*.ssa' | ForEach-Object {
    Write-Host ("rm " + $_.Name)
    Remove-Item $_.FullName -Force
}
Get-ChildItem -File -Filter '*.s' | ForEach-Object {
    Write-Host ("rm " + $_.Name)
    Remove-Item $_.FullName -Force
}
Get-ChildItem -File -Filter '*.o' | ForEach-Object {
    Write-Host ("rm " + $_.Name)
    Remove-Item $_.FullName -Force
}
Get-ChildItem -File -Filter '*_rt.c' | ForEach-Object {
    Write-Host ("rm " + $_.Name)
    Remove-Item $_.FullName -Force
}

# The native build temp directory.
if (Test-Path '_vayu_tmp') {
    Remove-Item '_vayu_tmp' -Recurse -Force
    Write-Host 'rm _vayu_tmp/'
}

# Any stray .exe in the repo root that isn't a keeper.
Get-ChildItem -File -Filter '*.exe' | ForEach-Object {
    if ($kept -notcontains $_.Name) {
        Write-Host ("rm " + $_.Name)
        Remove-Item $_.FullName -Force
    }
}

if ($Full) {
    foreach ($f in $kept) {
        if (Test-Path $f) {
            Write-Host ("rm " + $f)
            Remove-Item $f -Force
        }
    }
}

Write-Host ''
Write-Host 'Clean.'
Pop-Location