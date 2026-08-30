# Test runner for pyluna interpreter — dumps all outputs to a single file
# so you can visually verify correctness (nulls, errors, etc.)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

$Exe = ".\pyluna.exe"
if (-not (Test-Path $Exe)) {
    Write-Host "Error: pyluna.exe not found. Run 'make' first." -ForegroundColor Red
    exit 1
}

$OutFile = "tests_py\tests_py_output.txt"
Remove-Item $OutFile -ErrorAction SilentlyContinue

$Passed = 0
$Failed = 0

Write-Host "Running pyluna regression tests..." -ForegroundColor Cyan

Get-ChildItem "tests_py\*.py" | ForEach-Object {
    $testName = $_.Name
    $shouldFail = $testName -like '*.should_fail.*'
    Write-Host -NoNewline "Running $testName... "

    # Merge stdout + stderr
    $tmpFile = [System.IO.Path]::GetTempFileName()
    & cmd /c "$Exe $($_.FullName) >$tmpFile 2>&1"
    $exitCode = $LASTEXITCODE
    $output = Get-Content $tmpFile -Raw -ErrorAction SilentlyContinue
    Remove-Item $tmpFile -ErrorAction SilentlyContinue

    # Strip DEBUG: lines from output (keep errors/vm messages)
    $cleanOutput = $output
    if ($cleanOutput) { $cleanOutput = $cleanOutput.TrimEnd() }

    Add-Content -Path $OutFile -Value "===== $testName ====="
    Add-Content -Path $OutFile -Value $cleanOutput
    Add-Content -Path $OutFile -Value ""

    if ($shouldFail) {
        if ($exitCode -ne 0) {
            Write-Host "PASSED (expected failure)" -ForegroundColor Green
            $Passed++
        } else {
            Write-Host "FAILED (expected failure but passed)" -ForegroundColor Red
            $Failed++
        }
    } else {
        if ($exitCode -eq 0) {
            Write-Host "PASSED" -ForegroundColor Green
            $Passed++
        } else {
            Write-Host "FAILED (exit code: $exitCode)" -ForegroundColor Red
            $Failed++
        }
    }
}

Write-Host "================================"
Write-Host "Results: $Passed passed, $Failed failed" -ForegroundColor $(if ($Failed -eq 0) { "Green" } else { "Red" })
Write-Host "Full output written to: $OutFile"
Write-Host ""
Get-Content $OutFile | Write-Host

if ($Failed -gt 0) { exit 1 }
exit 0