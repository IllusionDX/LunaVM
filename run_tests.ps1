# Test runner for Luna interpreter regression tests
# Runs all .luna files in the tests/ directory

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

$Exe = ".\luna.exe"
if (-not (Test-Path $Exe)) {
    Write-Host "Error: luna.exe not found. Run 'make' first." -ForegroundColor Red
    exit 1
}

$Passed = 0
$Failed = 0

Write-Host "Running Luna regression tests..." -ForegroundColor Cyan
Write-Host "================================"

Get-ChildItem "tests\*.luna" | ForEach-Object {
    $testName = $_.Name
    Write-Host -NoNewline "Running $testName... "

    $output = & $Exe $_.FullName 2>&1
    $exitCode = $LASTEXITCODE

    if ($exitCode -eq 0) {
        Write-Host "PASSED" -ForegroundColor Green
        $Passed++
    }
    else {
        Write-Host "FAILED (exit code: $exitCode)" -ForegroundColor Red
        $output | Select-Object -First 10
        $Failed++
    }
}

Write-Host "================================"
Write-Host "Results: $Passed passed, $Failed failed" -ForegroundColor $(if ($Failed -eq 0) { "Green" } else { "Red" })

if ($Failed -gt 0) {
    exit 1
}
exit 0