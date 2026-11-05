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

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName PresentationCore
Add-Type -AssemblyName PresentationFramework
Add-Type -AssemblyName WindowsBase
$zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Package).Path)
try {
    foreach ($entry in $zip.Entries) {
        Assert ($entry.FullName -notmatch '(?i)(\.xbe$|\.cpp$|\.o$|libscda_code\.so$|(^|[\\/])work/)') "Game code/asset in package: $($entry.FullName)"
    }
} finally { $zip.Dispose() }
Expand-Archive -LiteralPath (Resolve-Path -LiteralPath $Package).Path -DestinationPath $root
$assembly = [Reflection.Assembly]::LoadFile((Join-Path $root 'SCDA-Android-ARM64-Builder.exe'))
Assert ($assembly.GetManifestResourceNames().Count -eq 0) 'GUI still embeds a compiler payload.'
$references = @($assembly.GetReferencedAssemblies() | ForEach-Object { $_.Name })
Assert ($references -contains 'PresentationFramework') 'Builder does not reference WPF.'
Assert ($references -notcontains 'System.Windows.Forms') 'Builder still references WinForms.'
Assert ((Get-Item "$root/SCDA-Android-ARM64-Builder.exe").Length -lt 1MB) 'GUI executable unexpectedly large.'
$window = [Activator]::CreateInstance($assembly.GetType('BuilderWindow', $true), $true)
try {
    $window.Show()
    [Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke([Action] {})
    Assert ($window.IsVisible -and $window.Title -eq 'SCDA Android ARM64 Builder') 'GUI window did not open.'
    $captions = [Collections.Generic.List[string]]::new()
    function Find-Captions($control) {
        if ($control -is [Windows.Controls.ContentControl] -and $control.Content -is [string]) { $captions.Add($control.Content) }
        if ($control -is [Windows.Controls.Panel]) { foreach ($child in $control.Children) { Find-Captions $child } }
        if ($control -is [Windows.Controls.ContentControl] -and $control.Content -isnot [string] -and $control.Content) { Find-Captions $control.Content }
    }
    Find-Captions $window.Content
    foreach ($caption in 'Folder...', 'ISO...', 'Save as...', 'Build game ZIP', 'Tools / licenses') {
        Assert ($captions.Contains($caption)) "Missing GUI control: $caption"
    }
} finally { $window.Close() }
$builder = $assembly.GetType('Builder', $true)
$flags = [Reflection.BindingFlags]'Static,NonPublic'
function Invoke-Builder([string]$method, [object[]]$parameters) {
    $values = [object[]]::new($parameters.Count)
    for ($i = 0; $i -lt $parameters.Count; $i++) { $values[$i] = $parameters[$i].PSObject.BaseObject }
    try { return $builder.GetMethod($method, $flags).Invoke($null, $values) }
    catch {
        $error = $_.Exception.InnerException
        throw "$method failed: $($error.Message)"
    }
}
Assert (Test-Path "$root/compiler/xbe_recomp.exe") 'Missing companion compiler.'
Assert (Test-Path "$root/extract-xiso.exe") 'Missing extract-xiso tool.'
Assert (Test-Path "$root/licenses/extract-xiso.txt") 'Missing extract-xiso license.'
Assert (Test-Path "$root/package-game.ps1") 'Missing companion ZIP helper.'
New-Item -ItemType Directory -Force -Path "$root/assets/Default", "$root/assets/Media" | Out-Null

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
    Set-Content -LiteralPath "$root/assets/Default/test.bin", "$root/assets/Media/test.bin" -Value 'synthetic game data'
    $importZip = Join-Path $root 'SCDA-game.zip'
    $logger = [Delegate]::CreateDelegate([Action[string]], [Console].GetMethod('WriteLine', [type[]]@([string])))
    Invoke-Builder 'BuildGame' @("$root/assets", $importZip, $root, $logger)
    Assert (Test-Path -LiteralPath $importZip) 'GUI build pipeline did not create import ZIP.'
    $gameZip = [IO.Compression.ZipFile]::OpenRead($importZip)
    try {
        Assert ($gameZip.Entries.Count -eq 4) 'Unexpected import ZIP shape.'
        Assert ($gameZip.GetEntry('libscda_code.so').Length -gt 1MB) 'Missing library in import ZIP.'
    } finally { $gameZip.Dispose() }
    # Build an XISO without media patching, so default.xbe retains the supported hash.
    $isoPath = Join-Path $root 'game test.iso'
    Push-Location $root
    try { & "$root/extract-xiso.exe" -q -m -c assets 'game test.iso' } finally { Pop-Location }
    Assert ($LASTEXITCODE -eq 0 -and (Test-Path $isoPath)) 'Creating the test XISO failed.'
    $isoHash = (Get-FileHash -LiteralPath $isoPath).Hash
    $isoZip = Join-Path $root 'from ISO.zip'
    Invoke-Builder 'BuildGame' @($isoPath, $isoZip, $root, $logger)
    Assert ((Get-FileHash -LiteralPath $isoPath).Hash -eq $isoHash) 'Source ISO was modified.'
    Assert (@(Get-ChildItem -LiteralPath $root -Directory -Filter 'iso-*').Count -eq 0) 'ISO extraction was not cleaned up.'
    $gameZip = [IO.Compression.ZipFile]::OpenRead($isoZip)
    try {
        Assert ($gameZip.Entries.Count -eq 4) 'Unexpected ISO-to-ZIP shape.'
        $xbeEntry = $gameZip.GetEntry('assets/default.xbe').Open()
        try {
            $sha = [Security.Cryptography.SHA1]::Create()
            try { $actualXbeHash = [BitConverter]::ToString($sha.ComputeHash($xbeEntry)).Replace('-', '') }
            finally { $sha.Dispose() }
        } finally { $xbeEntry.Dispose() }
        Assert ($actualXbeHash -eq (Get-FileHash -LiteralPath $Xbe -Algorithm SHA1).Hash) 'ISO extraction changed the XBE.'
    } finally { $gameZip.Dispose() }
    $outputHash = (Get-FileHash -LiteralPath $isoZip).Hash
    $invalidIso = Join-Path $root 'invalid.iso'
    Set-Content -LiteralPath $invalidIso -Value 'not an ISO'
    try {
        Invoke-Builder 'BuildGame' @($invalidIso, $isoZip, $root, $logger)
        throw 'Invalid ISO was accepted.'
    } catch { Assert ($_.Exception.Message -match 'extract-xiso.exe failed') 'Wrong invalid ISO error.' }
    Assert ((Get-FileHash -LiteralPath $isoZip).Hash -eq $outputHash) 'Invalid ISO damaged previous ZIP.'
    Assert (@(Get-ChildItem -LiteralPath $root -Directory -Filter 'iso-*').Count -eq 0) 'Failed ISO extraction was not cleaned up.'
    try {
        Invoke-Builder 'ValidatePaths' @("$root/assets", "$root/assets/game.zip")
        throw 'Output inside source folder was accepted.'
    } catch { Assert ($_.Exception.Message -match 'outside your source') 'Wrong output validation error.' }
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
    Write-Host "PASS: GUI window/controls, small EXE with companion tools, folder/ISO build-to-ZIP, ISO immutability/cleanup/rejection, path validation, missing/invalid XBE, full ARM64 build, exports, host dependency, inline atomics, no-op rebuild and resume."
    Write-Host "Test output retained at $root/libscda_code.so"
} finally {
    $env:PATH = $oldPath
}
