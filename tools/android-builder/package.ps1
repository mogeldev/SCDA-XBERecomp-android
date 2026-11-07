param(
    [Parameter(Mandatory = $true)][string]$Recompiler,
    [Parameter(Mandatory = $true)][string]$Xbe,
    [string]$SdkRoot = $env:ANDROID_HOME
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Run after building xbe_recomp on Windows. Pair the package with an APK from the same revision.
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$Recompiler = (Resolve-Path -LiteralPath $Recompiler).Path
$Xbe = (Resolve-Path -LiteralPath $Xbe).Path
$llvm = Join-Path $SdkRoot 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin'
$cmake = Join-Path $SdkRoot 'cmake/3.31.6'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -property installationPath
if ($LASTEXITCODE -or -not $vs) { throw 'Visual Studio C++ redistributable files not found.' }
$crt = @(Get-ChildItem "$vs/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT" -Directory | Sort-Object FullName -Descending)[0].FullName
$ndk = Join-Path $SdkRoot 'ndk/29.0.14206865'
# One version source: the APK's versionName. The builder and the APK are published together under it.
$gradle = Get-Content -LiteralPath (Join-Path $repo 'android/build.gradle') -Raw
if ($gradle -notmatch "versionName\s*=\s*'([0-9A-Za-z.\-]+)'") { throw 'versionName not found in android/build.gradle.' }
$version = $Matches[1]

$stage = Join-Path $repo ('out/android-builder-package-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path "$stage/compiler/include/cpu", "$stage/compiler/include/freestanding", "$stage/compiler/config", "$stage/licenses" | Out-Null
$temp = $null
try {
    Copy-Item -LiteralPath $Recompiler -Destination "$stage/compiler/xbe_recomp.exe"
    # Link-only stub: the phone loads the real host from the matching APK via DT_NEEDED.
    Set-Content -LiteralPath "$stage/host.c" -Value 'void scda_builder_stub(void) {}' -Encoding ASCII
    & "$llvm/clang.exe" --target=aarch64-linux-android30 -fPIC -c "$stage/host.c" -o "$stage/host.o"
    if ($LASTEXITCODE) { throw 'Compiling the host link stub failed.' }
    & "$llvm/ld.lld.exe" -shared -soname libscda_host.so "$stage/host.o" -o "$stage/compiler/libscda_host.so"
    if ($LASTEXITCODE) { throw 'Linking the host stub failed.' }
    Remove-Item -LiteralPath "$stage/host.c", "$stage/host.o"
    foreach ($name in 'clang++.exe', 'ld.lld.exe', 'libwinpthread-1.dll', 'libxml2.dll') {
        Copy-Item -LiteralPath "$llvm/$name" -Destination "$stage/compiler/$name"
    }
    Copy-Item -LiteralPath "$cmake/bin/ninja.exe" -Destination "$stage/compiler/ninja.exe"
    foreach ($name in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
        Copy-Item -LiteralPath "$crt/$name" -Destination "$stage/compiler/$name"
    }
    Copy-Item -LiteralPath "$repo/config/scda_v2.toml", "$repo/config/xdk_symbols.toml" -Destination "$stage/compiler/config"
    Copy-Item -LiteralPath "$repo/src/cpu/x86_ops.h", "$repo/src/cpu/x86_context.h" -Destination "$stage/compiler/include/cpu"
    Copy-Item "$repo/src/cpu/freestanding/*" -Destination "$stage/compiler/include/freestanding"
    Copy-Item -LiteralPath "$PSScriptRoot/Builder.cs", "$PSScriptRoot/build.ps1", "$PSScriptRoot/package-game.ps1", "$PSScriptRoot/README.md" -Destination $stage
    # Read by the GUI and written into every game ZIP; the APK refuses a ZIP from another version.
    Set-Content -LiteralPath "$stage/version.txt" -Value $version -Encoding ASCII
    # Build the pinned extract-xiso source for the Windows host, not Android.
    $extractBuild = Join-Path $repo 'out/android-extract-xiso'
    & "$cmake/bin/cmake.exe" -S "$PSScriptRoot/extract-xiso" -B $extractBuild -G 'Visual Studio 17 2022' -A x64
    if ($LASTEXITCODE) { throw 'Configuring extract-xiso failed.' }
    & "$cmake/bin/cmake.exe" --build $extractBuild --config Release --parallel
    if ($LASTEXITCODE) { throw 'Building extract-xiso failed.' }
    Copy-Item -LiteralPath "$extractBuild/Release/extract-xiso.exe" -Destination $stage
    Copy-Item -LiteralPath "$repo/LICENSE" -Destination "$stage/licenses/SCDA.txt"
    Copy-Item -LiteralPath "$repo/tools/LLVM-LICENSE.txt" -Destination "$stage/licenses/LLVM.txt"
    Copy-Item -LiteralPath "$extractBuild/extract-xiso-LICENSE.txt" -Destination "$stage/licenses/extract-xiso.txt"
    Copy-Item -LiteralPath "$ndk/NOTICE", "$ndk/NOTICE.toolchain" -Destination "$stage/licenses"
    Copy-Item -LiteralPath "$cmake/doc/ninja/LICENSE" -Destination "$stage/licenses/Ninja.txt"
    # xbe_recomp statically links Zydis, Zycore and toml++.
    foreach ($dependency in 'zydis', 'zycore', 'tomlplusplus') {
        $license = Get-ChildItem -LiteralPath (Split-Path (Split-Path $Recompiler)) -Recurse -File -Filter LICENSE |
            Where-Object { $_.FullName.Replace('\', '/') -match "/$dependency(-src)?/LICENSE$" } | Select-Object -First 1
        if (-not $license) { throw "Could not find the $dependency license under the recompiler build directory." }
        Copy-Item -LiteralPath $license.FullName -Destination "$stage/licenses/$dependency.txt"
    }
    Set-Content -LiteralPath "$stage/compiler/xbe.sha1" -Value (Get-FileHash -LiteralPath $Xbe -Algorithm SHA1).Hash -Encoding ASCII
    $csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    if (-not (Test-Path -LiteralPath $csc)) { $csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe' }
    if (-not (Test-Path -LiteralPath $csc)) { throw 'Windows .NET Framework compiler (csc.exe) was not found.' }
    $gui = Join-Path $stage 'SCDA-Android-ARM64-Builder.exe'
    $source = Join-Path $stage 'Builder.cs'
    $wpf = Join-Path (Split-Path $csc) 'WPF'
    & $csc /nologo /target:winexe /optimize+ /out:$gui `
        /reference:System.dll /reference:System.Core.dll /reference:System.Xaml.dll `
        /reference:"$wpf/WindowsBase.dll" /reference:"$wpf/PresentationCore.dll" /reference:"$wpf/PresentationFramework.dll" $source
    if ($LASTEXITCODE) { throw 'Compiling the builder GUI failed.' }
    $output = Join-Path $repo "out/SCDA-Android-$version-Windows-Builder.zip"
    $temp = Join-Path $repo ('out/' + [Guid]::NewGuid().ToString('N') + '.zip')
    Compress-Archive -Path "$stage/*" -DestinationPath $temp -CompressionLevel Optimal
    Move-Item -LiteralPath $temp -Destination $output -Force
    Write-Host "Created $output (builder ${version}: GUI with companion tools; no game assets or generated code)."
} finally {
    if ($temp -and (Test-Path -LiteralPath $temp)) { Remove-Item -LiteralPath $temp -Force }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
