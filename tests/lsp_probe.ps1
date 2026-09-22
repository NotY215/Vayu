# tests\lsp_probe.ps1 — single probe for every LSP feature.
param(
    [string]$Vls = "build\x64-debug\bin\vls.exe"
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = (Resolve-Path $Vls).Path
$psi.RedirectStandardInput  = $true
$psi.RedirectStandardOutput = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow  = $true

$proc = [System.Diagnostics.Process]::Start($psi)

function Send([string]$json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $header = "Content-Length: " + $bytes.Length + "`r`n`r`n"
    $proc.StandardInput.Write($header)
    $proc.StandardInput.Write($json)
    $proc.StandardInput.Flush()
}

Send '{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"capabilities":{}}}'

$source = 'class Point:\n    x: int\n    def __init__(self, x: int) -> None:\n        self.x = x\n\ndef distance(p: Point) -> int:\n    return p.x\n\ndef main() -> None:\n    p = Point(5)\n    print(distance(p))\n'
$openJson = '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///x.vyu","text":"' + $source + '"}}}'
Send $openJson

# line 10 = "    print(distance(p))"  -- d of distance at col 10, p inside at col 19
Send '{"jsonrpc":"2.0","id":1,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///x.vyu"},"position":{"line":10,"character":10}}}'
Send '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///x.vyu"},"position":{"line":0,"character":7}}}'
Send '{"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///x.vyu"},"position":{"line":10,"character":10}}}'
Send '{"jsonrpc":"2.0","id":4,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///x.vyu"},"position":{"line":5,"character":5},"context":{"includeDeclaration":true}}}'
Send '{"jsonrpc":"2.0","id":5,"method":"textDocument/rename","params":{"textDocument":{"uri":"file:///x.vyu"},"position":{"line":5,"character":5},"newName":"dist"}}'
Send '{"jsonrpc":"2.0","id":6,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///x.vyu"},"position":{"line":10,"character":11}}}'
Send '{"jsonrpc":"2.0","id":7,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///x.vyu"},"position":{"line":10,"character":19}}}'

Start-Sleep -Milliseconds 800

Send '{"jsonrpc":"2.0","id":99,"method":"shutdown","params":null}'
Send '{"jsonrpc":"2.0","method":"exit","params":null}'

$proc.StandardInput.Close()
$raw = $proc.StandardOutput.ReadToEnd()
$proc.WaitForExit()

$messages = @()
$i = 0
$haystack = $raw -replace "`r`n", "`n"
while ($i -lt $haystack.Length) {
    $hdrEnd = $haystack.IndexOf("`n`n", $i)
    if ($hdrEnd -lt 0) { break }
    $hdr = $haystack.Substring($i, $hdrEnd - $i)
    $len = 0
    foreach ($line in $hdr -split "`n") {
        if ($line -like "Content-Length:*") {
            $len = [int](($line.Substring(15)).Trim())
        }
    }
    $bodyStart = $hdrEnd + 2
    if ($bodyStart + $len -gt $haystack.Length) { break }
    $body = $haystack.Substring($bodyStart, $len)
    $messages += $body
    $i = $bodyStart + $len
}

$names = @{
    0  = "initialize response"
    1  = "hover on function"
    2  = "hover on class"
    3  = "goto definition"
    4  = "references"
    5  = "rename"
    6  = "completion"
    7  = "signature help"
    99 = "shutdown response"
}

Write-Host ""
Write-Host "LSP probe ($($messages.Count) messages)" -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan

$okCount = 0
$nullCount = 0
foreach ($m in $messages) {
    $o = $m | ConvertFrom-Json
    $tag = ""
    if ($o.method -eq "textDocument/publishDiagnostics") {
        $tag = "publishDiagnostics"
    } elseif ($null -ne $o.id) {
        $id = [int]$o.id
        if ($names.ContainsKey($id)) { $tag = $names[$id] } else { $tag = "id=$id" }
    }
    if ($null -ne $o.id -and $names.ContainsKey([int]$o.id) -and
        [int]$o.id -ne 0 -and [int]$o.id -ne 99) {
        if ($null -eq $o.result) { $nullCount++ } else { $okCount++ }
    }
    Write-Host ("--- {0} ---" -f $tag) -ForegroundColor Yellow
    Write-Host ($m | ConvertTo-Json -Depth 20 -Compress)
    Write-Host ""
}

Write-Host ""
Write-Host ("Functional responses: {0} populated, {1} null" -f $okCount, $nullCount) -ForegroundColor Cyan
Write-Host ("Raw bytes: {0}" -f $raw.Length) -ForegroundColor DarkGray

if ($messages.Count -lt 9) {
    Write-Host ""
    Write-Host "NOT ALL MESSAGES ARRIVED. Raw stream follows." -ForegroundColor Red
    Write-Host "----------------------------------------------"
    Write-Host $raw
}