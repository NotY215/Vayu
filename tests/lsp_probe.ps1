$vls = "build\x64-debug\bin\vls.exe"
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = (Resolve-Path $vls).Path
$psi.RedirectStandardInput  = $true
$psi.RedirectStandardOutput = $true
$psi.UseShellExecute = $false
$psi.CreateNoWindow  = $true

$p = [System.Diagnostics.Process]::Start($psi)

function Send-Lsp([string]$json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $header = "Content-Length: " + $bytes.Length + "`r`n`r`n"
    $p.StandardInput.Write($header)
    $p.StandardInput.Write($json)
    $p.StandardInput.Flush()
}

$init = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
$open = '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///x.vyu","text":"def f(:\n    pass\n"}}}'

Send-Lsp $init
Send-Lsp $open
Start-Sleep -Milliseconds 500
$p.StandardInput.Close()
$out = $p.StandardOutput.ReadToEnd()
$p.WaitForExit()

Write-Host "--- vls output ---"
Write-Host $out
Write-Host "--- exit code: $($p.ExitCode) ---"