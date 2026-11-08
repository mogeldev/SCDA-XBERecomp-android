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
    Set-Content -LiteralPath "$root/version.txt" -Value '0.1.0-testing.3' -Encoding ASCII
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Assets "$root/assets" -Library "$root/libscda_code.so" -Output "$root/versioned.zip" -VersionFile "$root/version.txt"
    if ($LASTEXITCODE) { throw 'Versioned packaging failed.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $versioned = [IO.Compression.ZipFile]::OpenRead("$root/versioned.zip")
    try {
        $reader = [IO.StreamReader]::new($versioned.GetEntry('scda-version.txt').Open())
        try { if ($reader.ReadToEnd().Trim() -ne '0.1.0-testing.3') { throw 'Wrong builder version in ZIP.' } } finally { $reader.Dispose() }
    } finally { $versioned.Dispose() }
    $zip = [IO.Compression.ZipFile]::OpenRead("$root/game.zip")
    try {
        $actual = @($zip.Entries | ForEach-Object { $_.FullName } | Sort-Object)
        $expected = @('libscda_code.so', 'assets/default.xbe', 'assets/Default/sub/data', 'assets/Media/video') | Sort-Object
        if (@(Compare-Object $actual $expected).Count) { throw "Unexpected ZIP entries: $actual" }
        if ($zip.GetEntry('assets/Default/sub/data').Length -eq 0) { throw 'Empty data entry.' }
    } finally { $zip.Dispose() }
    New-Item -ItemType Directory -Force -Path "$root/lib/arm64-v8a", "$root/lib/x86_64" | Out-Null
    foreach ($abi in 'arm64-v8a', 'x86_64') { Copy-Item "$root/libscda_code.so" "$root/lib/$abi/libscda_code.so" }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Assets "$root/assets" -Library "$root/lib" -Output "$root/universal.zip" -VersionFile "$root/version.txt"
    if ($LASTEXITCODE) { throw 'Dual-ABI packaging failed.' }
    $dual = [IO.Compression.ZipFile]::OpenRead("$root/universal.zip")
    try {
        if ($dual.Entries.Count -ne 6 -or -not $dual.GetEntry('lib/arm64-v8a/libscda_code.so') -or -not $dual.GetEntry('lib/x86_64/libscda_code.so')) { throw 'Wrong dual-ABI ZIP layout.' }
    } finally { $dual.Dispose() }
    $beforeDual = (Get-FileHash "$root/universal.zip").Hash
    Remove-Item "$root/lib/x86_64/libscda_code.so"
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Assets "$root/assets" -Library "$root/lib" -Output "$root/universal.zip" 2>$null
    } finally { $ErrorActionPreference = $oldPreference }
    if ($LASTEXITCODE -eq 0 -or (Get-FileHash "$root/universal.zip").Hash -ne $beforeDual) { throw 'Missing ABI damaged the output or was accepted.' }
    $before = (Get-FileHash "$root/game.zip").Hash
    Remove-Item -LiteralPath "$root/assets/default.xbe"
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Assets "$root/assets" -Library "$root/libscda_code.so" -Output "$root/game.zip" 2>$null
    } finally { $ErrorActionPreference = $oldPreference }
    if ($LASTEXITCODE -eq 0) { throw 'Accepted missing XBE.' }
    if ((Get-FileHash "$root/game.zip").Hash -ne $before) { throw 'Damaged previous ZIP on failure.' }
    Write-Host 'PASS: import ZIP shape, builder version entry, spaces in paths, campaign-only filtering, content and failure preservation.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
