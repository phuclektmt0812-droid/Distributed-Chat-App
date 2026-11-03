param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

$resolved = Join-Path (Get-Location) $Executable
if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
    Write-Error "Test executable not found: $resolved"
    exit 2
}

& $resolved
exit $LASTEXITCODE
