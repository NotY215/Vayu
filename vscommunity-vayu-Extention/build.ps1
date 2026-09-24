# Build a native Visual Studio VSIX package for Vayu.
# This does not use vsce. vsce is for VS Code extensions.

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Output = Join-Path $Root "vayu-vs-community-0.2.0.vsix"
$Stage = Join-Path $Root ".vsix-stage"
$Zip = Join-Path $Root "vayu-vs-community-0.2.0.zip"

function FailIfMissing([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file not found: $Path"
    }
}

if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
if (Test-Path -LiteralPath $Output) { Remove-Item -LiteralPath $Output -Force }
if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }

$RequiredFiles = @(
    "extension.vsixmanifest",
    "Vayu.pkgdef",
    "Vayu_logo.png",
    "LICENSE.txt",
    "README.md",
    "Grammars\vayu.tmLanguage.json",
    "Grammars\language-configuration.json",
    "assets\Vyu_Icon.ico",
    "assets\Vyu_Icon_Dark.ico"
)

foreach ($File in $RequiredFiles) {
    FailIfMissing (Join-Path $Root $File)
}

New-Item -ItemType Directory -Path $Stage | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Stage "Grammars") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Stage "assets") | Out-Null

# VSIX package metadata must be at the package root.
$ContentTypes = @'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json" />
  <Default Extension="pkgdef" ContentType="text/plain" />
  <Default Extension="txt" ContentType="text/plain" />
  <Default Extension="md" ContentType="text/markdown" />
  <Default Extension="png" ContentType="image/png" />
  <Default Extension="ico" ContentType="image/x-icon" />
  <Override PartName="/extension.vsixmanifest" ContentType="text/xml" />
</Types>
'@

# Use .NET instead of Set-Content -Encoding so the script also works
# on older Windows PowerShell versions where that parameter is unavailable.
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    (Join-Path $Stage "[Content_Types].xml"),
    $ContentTypes,
    $Utf8NoBom
)

Copy-Item -LiteralPath (Join-Path $Root "extension.vsixmanifest") -Destination $Stage
Copy-Item -LiteralPath (Join-Path $Root "Vayu.pkgdef") -Destination $Stage
Copy-Item -LiteralPath (Join-Path $Root "Vayu_logo.png") -Destination $Stage
Copy-Item -LiteralPath (Join-Path $Root "LICENSE.txt") -Destination $Stage
Copy-Item -LiteralPath (Join-Path $Root "README.md") -Destination $Stage
Copy-Item -LiteralPath (Join-Path $Root "Grammars\vayu.tmLanguage.json") -Destination (Join-Path $Stage "Grammars")
Copy-Item -LiteralPath (Join-Path $Root "Grammars\language-configuration.json") -Destination (Join-Path $Stage "Grammars")
Copy-Item -LiteralPath (Join-Path $Root "assets\Vyu_Icon.ico") -Destination (Join-Path $Stage "assets")
Copy-Item -LiteralPath (Join-Path $Root "assets\Vyu_Icon_Dark.ico") -Destination (Join-Path $Stage "assets")

# The VSIX format is a ZIP/OPC package. Compress-Archive is available in
# Windows PowerShell 5+ and preserves the required package-root layout.
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $Zip -CompressionLevel Optimal

Move-Item -LiteralPath $Zip -Destination $Output
Remove-Item -LiteralPath $Stage -Recurse -Force

Write-Host ""
Write-Host "VSIX created successfully:" -ForegroundColor Green
Write-Host $Output
