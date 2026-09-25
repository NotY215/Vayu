# Build the Vayu Visual Studio 18+ VSIX using the official SDK-style VSSDK build pipeline.
# Requires .NET SDK and the NuGet packages restored by Vayu.Vsix.csproj.
# Do not use vsce and do not manually ZIP the VSIX.

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Project = Join-Path $Root "Vayu.Vsix.csproj"

if (-not (Test-Path -LiteralPath $Project -PathType Leaf)) {
    throw "Vayu.Vsix.csproj was not found: $Project"
}

$DotNet = Get-Command dotnet -ErrorAction SilentlyContinue
if ($null -eq $DotNet) {
    throw "dotnet was not found. Install the .NET SDK, then run this script again."
}

Write-Host "Building Vayu VSIX with Microsoft.VSSDK.BuildTools 18.5..." -ForegroundColor Cyan

& $DotNet.Source build $Project --configuration Release --nologo
if ($LASTEXITCODE -ne 0) {
    throw "VSIX build failed with exit code $LASTEXITCODE."
}

$Output = Join-Path $Root "bin\Release\vayu-vs-community-0.3.0.vsix"

if (-not (Test-Path -LiteralPath $Output -PathType Leaf)) {
    $Candidates = Get-ChildItem -LiteralPath (Join-Path $Root "bin\Release") -Filter "*.vsix" -File -ErrorAction SilentlyContinue
    if ($Candidates.Count -eq 1) {
        $Output = $Candidates[0].FullName
    } else {
        throw "Build completed, but the expected VSIX was not found in bin\Release."
    }
}

Write-Host ""
Write-Host "VSIX created successfully:" -ForegroundColor Green
Write-Host $Output
