# tests\run_all.ps1 — cross-backend sanity harness.
#
# Runs each top-level .vyu under examples/ through the tree-walker, VM, and
# native backends.  Where a backend succeeds, its stdout must match the
# tree-walker's; a mismatch is a regression.  Where a backend fails, it is
# noted as skipped (informational), not a failure.
#
# Run from anywhere; the script chdir's to the Vayu root automatically.

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$vayuc = "build\x64-debug\bin\vayuc.exe"
if (-not (Test-Path $vayuc)) {
    Write-Host "FATAL: $vayuc not found. Build the project first." -ForegroundColor Red
    exit 2
}
if (-not (Test-Path "tools\qbe.exe")) {
    Write-Host "FATAL: tools\qbe.exe not found (needed for --native)." -ForegroundColor Red
    exit 2
}

# Files that need stdin, are expected to fail, or are inputs to other tools.
$skip = @(
    "input.vyu",        # interactive
    "native_io.vyu",    # interactive
    "type_errors.vyu",  # expected to fail type-checking
    "expr.vyu",         # bare expressions, no output
    "vlex_test.vyu",    # input for vlex.exe, not runnable
    "vparse_test.vyu",  # input for vparse.exe
    "vcode_test.vyu",   # input for vcode.exe
    "lambdas.vyu",      # native backend does not yet support lambdas
    "vm_lambdas.vyu",   # native backend does not yet support lambdas
    "stdlib.vyu",       # uses math module, unsupported in native
    "native_collections.vyu",  # prints whole maps; iteration order is impl-defined
    "fs_test.vyu",             # native-only; interp/VM don't yet support the fs module
    "time_test.vyu",           # native-only; same reason
    "json_test.vyu",           # native-only; same reason
    "regex_test.vyu",          # native-only; same reason
    "thread_test.vyu",         # native-only; same reason
    "net_test.vyu",            # native-only; same reason
    "crypto_test.vyu",         # native-only; same reason
    "random_test.vyu",         # native-only; same reason
    "os_test.vyu",             # native-only; same reason
    "ptr_arith_test.vyu",      # native-only; same reason
    "ffi_callback_test.vyu",   # native-only; same reason
    "py_init_test.vyu",        # native-only; same reason
    "py_bridge_test.vyu",      # native-only; same reason
    "py_call_test.vyu",        # native-only; same reason
    "py_bidi_test.vyu",        # native-only; same reason
    "py_close_test.vyu",       # native-only; same reason
    "py_eval_test.vyu",        # native-only; same reason
    "py_callback_test.vyu",    # native-only; same reason
    "ffi_test.vyu",
    "ffi_struct_test.vyu",
    "ffi_wrap_test.vyu",
    "math_test.vyu"            # tree/VM only; native backend has no float type yet
)

function Invoke-Backend {
    param($file, $mode)
    $tmp = [System.IO.Path]::GetTempFileName()
    $argList = @($file)
    if ($mode) { $argList += $mode }
    & $vayuc @argList *> $tmp
    $rc = $LASTEXITCODE
    $out = Get-Content $tmp -Raw -ErrorAction SilentlyContinue
    Remove-Item -Force $tmp -ErrorAction SilentlyContinue
    if ($null -eq $out) { $out = "" }
    return @{ rc = $rc; out = $out }
}

$passCount = 0
$failCount = 0
$skipCount = 0
$problems  = @()

$examples = Get-ChildItem -Path "examples" -Filter "*.vyu" -File | Sort-Object Name

Write-Host ""
Write-Host "Vayu cross-backend harness" -ForegroundColor Cyan
Write-Host "==========================" -ForegroundColor Cyan

foreach ($f in $examples) {
    $name = $f.Name
    $rel  = "examples\" + $name

    if ($skip -contains $name) {
        Write-Host ("[skip]  {0}" -f $name) -ForegroundColor DarkGray
        $skipCount++
        continue
    }

    $tw = Invoke-Backend $rel ""
    if ($tw.rc -ne 0) {
        Write-Host ("[FAIL]  {0}  (tree-walk rc={1})" -f $name, $tw.rc) -ForegroundColor Red
        $failCount++
        $problems += ("{0} (tree-walk rc={1})" -f $name, $tw.rc)
        continue
    }

    $vm  = Invoke-Backend $rel "--vm"
    $nat = Invoke-Backend $rel "--native"

    $ok = $true
    if ($vm.rc -eq 0 -and $vm.out -ne $tw.out) {
        Write-Host ("[FAIL]  {0}  (VM output differs)" -f $name) -ForegroundColor Red
        $ok = $false
    }
    if ($nat.rc -eq 0 -and $nat.out -ne $tw.out) {
        Write-Host ("[FAIL]  {0}  (native output differs)" -f $name) -ForegroundColor Red
        $ok = $false
    }

    if ($ok) {
        $tags = "tree"
        if ($vm.rc -eq 0)  { $tags += "+vm" }
        if ($nat.rc -eq 0) { $tags += "+native" }
        Write-Host ("[ok]    {0}  ({1})" -f $name, $tags) -ForegroundColor Green
        $passCount++
    } else {
        $failCount++
        $problems += ("{0} (output mismatch)" -f $name)
    }
}

Write-Host ""
Write-Host "--------------------------"
Write-Host ("Pass: {0}" -f $passCount) -ForegroundColor Green
if ($failCount -gt 0) {
    Write-Host ("Fail: {0}" -f $failCount) -ForegroundColor Red
} else {
    Write-Host "Fail: 0" -ForegroundColor Gray
}
Write-Host ("Skip: {0}" -f $skipCount) -ForegroundColor DarkGray

if ($failCount -gt 0) {
    Write-Host ""
    Write-Host "Failures:" -ForegroundColor Red
    foreach ($p in $problems) { Write-Host ("  " + $p) -ForegroundColor Red }
    exit 1
}

# --- Fixpoint checks: vcode / vayu must compile their own source byte-identically.

#
# IMPORTANT: PowerShell's `>` / `*>` operators write UTF-16LE with a BOM, which
# corrupts the .ssa file (QBE sees byte 0xFF at line 1).  We redirect via
# `cmd /c "… > file"` so cmd.exe writes raw ASCII bytes with no BOM.

Write-Host ""
Write-Host "Fixpoint checks" -ForegroundColor Cyan
Write-Host "---------------" -ForegroundColor Cyan

function Test-Fixpoint {
    param([string]$compiler, [string]$source, [string]$label)

    if (-not (Test-Path $compiler)) {
        Write-Host ("  {0}: {1} missing, skipping" -f $label, $compiler) -ForegroundColor DarkGray
        return $true
    }

    $ssa1 = "${label}_self.ssa"
    $asm  = "${label}_self.s"
    $exe  = "${label}_self.exe"
    $ssa2 = "${label}_self2.ssa"

    # 1. Compile source with the named compiler.
    cmd /c "`"$compiler`" `"$source`" > `"$ssa1`" 2>nul"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ssa1)) {
        Write-Host ("  {0}: first compile failed (rc={1})" -f $label, $LASTEXITCODE) -ForegroundColor Red
        return $false
    }

    # 2. qbe → .s
    cmd /c "tools\qbe.exe -t amd64_win -o `"$asm`" `"$ssa1`" 2>nul"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $asm)) {
        Write-Host ("  {0}: qbe failed" -f $label) -ForegroundColor Red
        return $false
    }

    # 3. gcc → exe
    cmd /c "gcc -O2 `"$asm`" vayu_rt.c -o `"$exe`" 2>nul"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        Write-Host ("  {0}: link failed" -f $label) -ForegroundColor Red
        return $false
    }

    # 4. Self-compile the same source with the new exe.
    cmd /c "`"$exe`" `"$source`" > `"$ssa2`" 2>nul"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ssa2)) {
        Write-Host ("  {0}: self-compile failed (rc={1})" -f $label, $LASTEXITCODE) -ForegroundColor Red
        return $false
    }

    # 5. Byte-compare the two IL files.
    $a = Get-Content $ssa1 -Raw
    $b = Get-Content $ssa2 -Raw
    if ($null -ne $a -and $null -ne $b -and $a -eq $b) {
        Write-Host ("  {0}: OK" -f $label) -ForegroundColor Green
        return $true
    }
    Write-Host ("  {0}: DIFFERS" -f $label) -ForegroundColor Red
    return $false
}

if (-not (Test-Fixpoint ".\vcode.exe" "vayu-src\vcode.vyu" "vcode")) { $failCount++ }
if (-not (Test-Fixpoint ".\vayu.exe"  "vayu-src\vayu.vyu"  "vayu"))  { $failCount++ }

Remove-Item -Force -ErrorAction SilentlyContinue `
    "vcode_self.ssa", "vcode_self.s", "vcode_self.exe", "vcode_self2.ssa", `
    "vayu_self.ssa",  "vayu_self.s",  "vayu_self.exe",  "vayu_self2.ssa"

if ($failCount -gt 0) { exit 1 }
Write-Host ""
Write-Host "All checks green." -ForegroundColor Green
exit 0