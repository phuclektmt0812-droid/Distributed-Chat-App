param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [string]$LogFile = "test-results.log"
)

$logPath = [System.IO.Path]::GetFullPath($LogFile)
[System.IO.File]::WriteAllText($logPath, "")

$resolved = Join-Path (Get-Location) $Executable
if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
    $message = "Test executable not found: $resolved"
    Write-Host $message
    Add-Content -LiteralPath $logPath -Value $message
    exit 2
}

$heading = "Running $Executable"
Write-Host $heading
Add-Content -LiteralPath $logPath -Value $heading

& $resolved 2>&1 | Tee-Object -FilePath $logPath -Append
$exitCode = $LASTEXITCODE
exit $exitCode
