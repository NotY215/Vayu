# Build a native Visual Studio VSIX package for Vayu.
# This does not use vsce. vsce is for VS Code extensions.

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Output = Join-Path $Root "vayu-vs-community-0.2.0.vsix"
$Stage = Join-Path $Root ".vsix-stage"
$Zip = Join-Path $Root "vayu-vs-community-0.2.0.zip"

if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
if (Test-Path $Output) { Remove-Item $Output -Force }
if (Test-Path $Zip) { Remove-Item $Zip -Force }

New-Item -ItemType Directory -Path $Stage | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Stage "Grammars") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Stage "assets") | Out-Null

# VSIX package metadata must be at the package root.
@'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="vsixmanifest" ContentType="text/xml" />
  <Default Extension="xml" ContentType="text/xml" />
  <Default Extension="json" ContentType="application/json" />
  <Default Extension="pkgdef" ContentType="text/plain" />
  <Default Extension="txt" ContentType="text/plain" />
  <Default Extension="md" ContentType="text/markdown" />
  <Default Extension="png" ContentType="image/png" />
  <Default Extension="ico" ContentType="image/x-icon" />
</Types>
'@ | Set-Content -Path (Join-Path $Stage "[Content_Types].xml") -Encoding UTF8

Copy-Item (Join-Path $Root "extension.vsixmanifest") $Stage
Copy-Item (Join-Path $Root "Vayu.pkgdef") $Stage
Copy-Item (Join-Path $Root "Vayu_logo.png") $Stage
Copy-Item (Join-Path $Root "LICENSE.txt") $Stage
Copy-Item (Join-Path $Root "README.md") $Stage
Copy-Item (Join-Path $Root "Grammars\vayu.tmLanguage.json") (Join-Path $Stage "Grammars")
Copy-Item (Join-Path $Root "Grammars\language-configuration.json") (Join-Path $Stage "Grammars")
Copy-Item (Join-Path $Root "assets\Vyu_Icon.ico") (Join-Path $Stage "assets")
Copy-Item (Join-Path $Root "assets\Vyu_Icon_Dark.ico") (Join-Path $Stage "assets")

Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $Zip -CompressionLevel Optimal
Move-Item $Zip $Output
Remove-Item $Stage -Recurse -Force

Write-Host ""
Write-Host "VSIX created:" -ForegroundColor Green
Write-Host $Output
