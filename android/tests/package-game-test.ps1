$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$root = Join-Path $repo ('out/import zip test ' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path "$root/assets/Default/sub", "$root/assets/Media", "$root/assets/adv" | Out-Null
try {
    Set-Content -LiteralPath "$root/assets/default.xbe" -Value 'XBEH' -Encoding ASCII
    Set-Content -LiteralPath "$root/assets/Default/sub/data" -Value 'data'
    Set-Content -LiteralPath "$root/assets/Media/video" -Value 'video'
    Set-Content -LiteralPath "$root/libscda_code.so" -Value 'library'
    Set-Content -LiteralPath "$root/assets/README.txt", "$root/assets/adv/versus", "$root/assets/sc4_adv.xbe" -Value 'excluded'
    $script = Join-Path $repo 'tools/android-builder/package-game.ps1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Assets "$root/assets" -Library "$root/libscda_code.so" -Output "$root/game.zip"
    if ($LASTEXITCODE) { throw 'Packaging failed.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead("$root/game.zip")
    try {
        $actual = @($zip.Entries | ForEach-Object { $_.FullName } | Sort-Object)
        $expected = @('libscda_code.so', 'assets/default.xbe', 'assets/Default/sub/data', 'assets/Media/video') | Sort-Object
        if (@(Compare-Object $actual $expected).Count) { throw "Unexpected ZIP entries: $actual" }
        if ($zip.GetEntry('assets/Default/sub/data').Length -eq 0) { throw 'Empty data entry.' }
    } finally { $zip.Dispose() }
    $before = (Get-FileHash "$root/game.zip").Hash
    Remove-Item -LiteralPath "$root/assets/default.xbe"
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Assets "$root/assets" -Library "$root/libscda_code.so" -Output "$root/game.zip" 2>$null
    } finally { $ErrorActionPreference = $oldPreference }
    if ($LASTEXITCODE -eq 0) { throw 'Accepted missing XBE.' }
    if ((Get-FileHash "$root/game.zip").Hash -ne $before) { throw 'Damaged previous ZIP on failure.' }
    Write-Host 'PASS: import ZIP shape, spaces in paths, campaign-only filtering, content and failure preservation.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
