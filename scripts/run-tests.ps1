param(
    [Parameter(Mandatory = $true)]
    [string]$TestDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Tests
)

$testNames = $Tests -split '\s+' | Where-Object { $_ }

foreach ($name in $testNames) {
    $executable = Join-Path $TestDirectory ($name + ".exe")

    Write-Host ""
    Write-Host "========== $name =========="

    if (-not (Test-Path -LiteralPath $executable)) {
        Write-Error "Test executable not found: $executable"
        exit 1
    }

    & $executable
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Test failed: $name (exit code $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

Write-Host ""
Write-Host "All $($testNames.Count) test programs passed."
