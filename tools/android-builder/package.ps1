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

$stage = Join-Path $repo ('out/android-builder-package-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path "$stage/compiler/include/cpu", "$stage/compiler/include/freestanding", "$stage/compiler/config", "$stage/assets", "$stage/licenses" | Out-Null
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
    Copy-Item -LiteralPath "$PSScriptRoot/build.bat", "$PSScriptRoot/build.ps1", "$PSScriptRoot/README.md" -Destination $stage
    Set-Content -LiteralPath "$stage/assets/README.txt" -Value 'Put your legally owned default.xbe here, or drag it onto build.bat.' -Encoding UTF8
    Copy-Item -LiteralPath "$repo/LICENSE" -Destination "$stage/licenses/SCDA.txt"
    Copy-Item -LiteralPath "$repo/tools/LLVM-LICENSE.txt" -Destination "$stage/licenses/LLVM.txt"
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
    $zip = Join-Path $repo 'out/SCDA-Android-ARM64-Builder.zip'
    $temp = Join-Path $repo ('out/' + [Guid]::NewGuid().ToString('N') + '.zip')
    Compress-Archive -Path "$stage/*" -DestinationPath $temp -CompressionLevel Optimal
    Move-Item -LiteralPath $temp -Destination $zip -Force
    Write-Host "Created $zip (no game assets, generated sources or libscda_code.so)."
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
