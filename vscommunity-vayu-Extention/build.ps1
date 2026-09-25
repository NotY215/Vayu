# Build the Vayu Visual Studio 18+ VSIX with the official VSSDK pipeline.
# On success, copy the generated VSIX to this extension folder and remove build artifacts.
# On failure, remove all build artifacts created by this script.

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Project = Join-Path $Root "Vayu.Vsix.csproj"
$Bin = Join-Path $Root "bin"
$Obj = Join-Path $Root "obj"
$RootVsix = Join-Path $Root "vayu-vs-community-0.3.0.vsix"

function Remove-BuildArtifacts {
    Write-Host ""
    Write-Host "Cleaning generated build files and folders..." -ForegroundColor Yellow

    foreach ($Path in @($Bin, $Obj)) {
        if (Test-Path -LiteralPath $Path) {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Host "Build artifacts cleaned." -ForegroundColor DarkGray
}

if (-not (Test-Path -LiteralPath $Project -PathType Leaf)) {
    throw "Vayu.Vsix.csproj was not found: $Project"
}

$DotNet = Get-Command dotnet -ErrorAction SilentlyContinue
if ($null -eq $DotNet) {
    throw "dotnet was not found. Install the .NET SDK, then run this script again."
}

$BuildSucceeded = $false

try {
    Write-Host "Building Vayu VSIX with Microsoft.VSSDK.BuildTools 18.5..." -ForegroundColor Cyan

    & $DotNet.Source build $Project --configuration Release --nologo
    if ($LASTEXITCODE -ne 0) {
        throw "VSIX build failed with exit code $LASTEXITCODE."
    }

    $Candidates = @(
        Get-ChildItem -LiteralPath (Join-Path $Root "bin\Release") -Filter "*.vsix" -File -Recurse -ErrorAction SilentlyContinue
    )

    if ($Candidates.Count -eq 0) {
        throw "Build completed, but no VSIX file was found under bin\Release."
    }

    if ($Candidates.Count -gt 1) {
        $Candidates = @($Candidates | Where-Object { $_.Name -eq "vayu-vs-community-0.3.0.vsix" })
    }

    if ($Candidates.Count -ne 1) {
        throw "Build completed, but the generated VSIX could not be identified uniquely."
    }

    $GeneratedVsix = $Candidates[0].FullName

    Copy-Item -LiteralPath $GeneratedVsix -Destination $RootVsix -Force

    $BuildSucceeded = $true

    Remove-BuildArtifacts

    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "VSIX BUILD PASSED" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "Generated VSIX:" -ForegroundColor Green
    Write-Host $RootVsix
}
catch {
    Remove-BuildArtifacts

    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "VSIX BUILD FAILED" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red

    exit 1
}
finally {
    if (-not $BuildSucceeded) {
        # The build script never removes source files or project files.
        # Only generated bin/obj artifacts are cleaned.
    }
}
