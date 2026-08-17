[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
$utf8Bom = New-Object System.Text.UTF8Encoding($true)
$gb18030 = [System.Text.Encoding]::GetEncoding(54936)

foreach ($sourceDir in 'Src', 'Inc') {
    $path = Join-Path $projectRoot $sourceDir
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        continue
    }

    Get-ChildItem -LiteralPath $path -File | Where-Object {
        $_.Extension -in '.c', '.h'
    } | ForEach-Object {
        $bytes = [System.IO.File]::ReadAllBytes($_.FullName)
        $hasUtf8Bom = $bytes.Length -ge 3 -and
            $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF

        if ($hasUtf8Bom) {
            return
        }

        try {
            $text = $utf8Strict.GetString($bytes)
        }
        catch {
            $text = $gb18030.GetString($bytes)
        }

        [System.IO.File]::WriteAllText($_.FullName, $text, $utf8Bom)
        Write-Host "UTF-8 BOM: $($_.FullName)"
    }
}
