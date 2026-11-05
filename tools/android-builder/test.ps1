param(
    [Parameter(Mandatory = $true)][string]$Package,
    [Parameter(Mandatory = $true)][string]$Xbe,
    [string]$SdkRoot = $env:ANDROID_HOME
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$root = Join-Path $repo ('out/builder smoke test ' + [Guid]::NewGuid().ToString('N'))
$readelf = Join-Path $SdkRoot 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe'
function Assert($condition, [string]$message) { if (-not $condition) { throw $message } }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Package).Path)
try {
    foreach ($entry in $zip.Entries) {
        Assert ($entry.FullName -notmatch '(?i)(\.xbe$|\.cpp$|\.o$|libscda_code\.so$|(^|/)work/)') "Game code/asset in package: $($entry.FullName)"
    }
} finally { $zip.Dispose() }
Expand-Archive -LiteralPath (Resolve-Path -LiteralPath $Package).Path -DestinationPath $root

# Use only Windows' PATH: players do not need a locally installed SDK, LLVM or compiler runtime.
$oldPath = $env:PATH
try {
    $env:PATH = "$env:SystemRoot/system32;$env:SystemRoot;$env:SystemRoot/System32/WindowsPowerShell/v1.0"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/build.ps1"
    Assert ($LASTEXITCODE -ne 0) 'Missing XBE was accepted.'
    Set-Content -LiteralPath "$root/assets/default.xbe" -Value 'not an xbe' -Encoding ASCII
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/build.ps1"
    Assert ($LASTEXITCODE -ne 0) 'Invalid XBE was accepted.'
    Copy-Item -LiteralPath (Resolve-Path -LiteralPath $Xbe).Path -Destination "$root/assets/default.xbe" -Force
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/build.ps1" -Jobs 8
    Assert ($LASTEXITCODE -eq 0) 'Full builder run failed.'
    $output = Get-Item -LiteralPath "$root/libscda_code.so"
    $bytes = [IO.File]::ReadAllBytes($output.FullName)
    Assert ($bytes[0] -eq 0x7f -and $bytes[1] -eq 0x45 -and $bytes[2] -eq 0x4c -and $bytes[3] -eq 0x46) 'Not an ELF library.'
    Assert ($bytes[4] -eq 2 -and $bytes[5] -eq 1 -and [BitConverter]::ToUInt16($bytes, 16) -eq 3 -and [BitConverter]::ToUInt16($bytes, 18) -eq 183) 'Not an ARM64 little-endian ELF64 shared library.'
    $dynamic = (& $readelf -d $output.FullName) -join "`n"
    Assert ($LASTEXITCODE -eq 0 -and $dynamic -match 'NEEDED.*\[libscda_host.so\]' -and $dynamic -match 'SONAME.*\[libscda_code.so\]') 'Missing host dependency or code SONAME.'
    $symbols = (& $readelf --dyn-syms $output.FullName) -join "`n"
    foreach ($symbol in 'X86FuncMappings', 'X86Continuations', 'X86CodeBase', 'X86CodeEnd', 'X86EntryPoint') {
        Assert ($symbols -match "GLOBAL\s+DEFAULT\s+\d+\s+$symbol\b") "Missing export: $symbol"
    }
    Assert ($symbols -notmatch 'UND\s+__aarch64_') 'Outlined atomic helpers are unresolved.'
    Assert ($symbols -match 'UND\s+__gxx_personality_v0') 'Expected shared exception runtime import missing.'
    $built = (Get-Item -LiteralPath "$root/work/libscda_code.so").LastWriteTimeUtc
    $again = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/build.ps1" -Jobs 8
    Assert ($LASTEXITCODE -eq 0 -and ($again -join "`n") -match 'no work to do') 'Second run was not incremental.'
    Assert ((Get-Item -LiteralPath "$root/work/libscda_code.so").LastWriteTimeUtc -eq $built) 'No-op run relinked the library.'
    # A cancelled/failed compilation must leave other completed objects reusable.
    Remove-Item -LiteralPath "$root/work/obj/x86_recomp.127.cpp.o"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$root/build.ps1" -Jobs 8
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path "$root/work/obj/x86_recomp.127.cpp.o")) 'Resuming with one missing object failed.'
    Write-Host "PASS: package contents, missing/invalid XBE, full ARM64 build, exports, host dependency, inline atomics, no-op rebuild and resume."
    Write-Host "Test output retained at $root/libscda_code.so"
} finally {
    $env:PATH = $oldPath
}
