param(
    [Parameter(Mandatory = $true)]
    [string]$TestDirectory,

    [Parameter(Mandatory = $true)]
    [string]$Tests,

    [string]$LogFile = "test-results.log"
)

$logPath = [System.IO.Path]::GetFullPath($LogFile)
[System.IO.File]::WriteAllText($logPath, "")
$testNames = $Tests -split '\s+' | Where-Object { $_ }

$started = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
$startMessage = "Test run started: $started"
Write-Host $startMessage
Add-Content -LiteralPath $logPath -Value $startMessage

foreach ($name in $testNames) {
    $executable = Join-Path $TestDirectory ($name + ".exe")

    $heading = "========== $name =========="
    Write-Host ""
    Write-Host $heading
    Add-Content -LiteralPath $logPath -Value ""
    Add-Content -LiteralPath $logPath -Value $heading

    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        $message = "Test executable not found: $executable"
        Write-Host $message
        Add-Content -LiteralPath $logPath -Value $message
        exit 1
    }

    & $executable 2>&1 | Tee-Object -FilePath $logPath -Append
    $exitCode = $LASTEXITCODE

    if ($exitCode -ne 0) {
        $message = "Test failed: $name (exit code $exitCode)"
        Write-Host $message
        Add-Content -LiteralPath $logPath -Value $message
        exit $exitCode
    }
}

$summary = "All $($testNames.Count) test programs passed."
Write-Host ""
Write-Host $summary
Add-Content -LiteralPath $logPath -Value ""
Add-Content -LiteralPath $logPath -Value $summary
