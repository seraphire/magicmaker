# setup.ps1
# Configure ESP-IDF environment variables

# Adjust this path to your ESP-IDF directory if needed
$Env:IDF_PATH = "C:\esp\v5.5.2\esp-idf"

if (-not (Test-Path $Env:IDF_PATH)) {
    Write-Error "IDF_PATH not found at $Env:IDF_PATH"
    exit 1
}

Push-Location $Env:IDF_PATH
. .\export.ps1
Pop-Location

Write-Host "ESP-IDF Environment Setup Complete" -ForegroundColor Green
