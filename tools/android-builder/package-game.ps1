param(
    [string]$Assets = (Join-Path $PSScriptRoot 'assets'),
    [string]$Library = (Join-Path $PSScriptRoot 'libscda_code.so'),
    [string]$Output = (Join-Path $PSScriptRoot 'SCDA-game.zip'),
    # the builder version, checked by the APK; absent in a source checkout
    [string]$VersionFile = (Join-Path $PSScriptRoot 'version.txt')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$assets = (Resolve-Path -LiteralPath $Assets).Path
$library = (Resolve-Path -LiteralPath $Library).Path
if ([IO.Path]::GetFileName($library) -ne 'libscda_code.so') { throw 'The library must be named libscda_code.so.' }
foreach ($name in 'default.xbe', 'Default', 'Media') {
    if (-not (Test-Path -LiteralPath (Join-Path $assets $name))) { throw "Missing assets/$name." }
}

$output = [IO.Path]::GetFullPath($Output)
if ([IO.Path]::GetFullPath($library) -eq $output) { throw 'Output must be a ZIP, not libscda_code.so.' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null
$temp = "$output.$([Guid]::NewGuid().ToString('N')).tmp"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
try {
    $zip = [IO.Compression.ZipFile]::Open($temp, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $buffer = New-Object byte[] (1MB)
        $written = 0L
        $watch = [Diagnostics.Stopwatch]::StartNew()
        function Add-File([string]$file, [string]$name) {
            $entry = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
            $source = [IO.File]::OpenRead($file)
            try {
                $target = $entry.Open()
                try {
                    while (($count = $source.Read($buffer, 0, $buffer.Length)) -gt 0) {
                        $target.Write($buffer, 0, $count)
                        $script:written += $count
                        if ($watch.ElapsedMilliseconds -ge 500) {
                            Write-Host ('ZIP: {0:N1} MiB packed | {1}' -f ($script:written / 1MB), $name)
                            $watch.Restart()
                        }
                    }
                } finally { $target.Dispose() }
            } finally { $source.Dispose() }
        }
        Add-File $library 'libscda_code.so'
        if (Test-Path -LiteralPath $VersionFile -PathType Leaf) { Add-File (Resolve-Path -LiteralPath $VersionFile).Path 'scda-version.txt' }
        Add-File (Join-Path $assets 'default.xbe') 'assets/default.xbe'
        foreach ($folder in 'Default', 'Media') {
            $base = Join-Path $assets $folder
            Get-ChildItem -LiteralPath $base -Recurse -File | ForEach-Object {
                $relative = $_.FullName.Substring($assets.Length).TrimStart('\', '/') -replace '\\', '/'
                Add-File $_.FullName ("assets/" + $relative)
            }
        }
    } finally { $zip.Dispose() }
    Move-Item -LiteralPath $temp -Destination $output -Force
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force }
}
Write-Host "Created $output"
