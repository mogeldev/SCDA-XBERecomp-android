param(
    [string]$Xbe = (Join-Path $PSScriptRoot 'assets/default.xbe'),
    [ValidateRange(0, 64)][int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

try {
    $compiler = Join-Path $PSScriptRoot 'compiler'
    foreach ($name in 'xbe_recomp.exe', 'clang++.exe', 'ld.lld.exe', 'ninja.exe', 'arm64-v8a/libscda_host.so', 'x86_64/libscda_host.so', 'arm64-v8a/libunwind.a', 'x86_64/libunwind.a', 'xbe.sha1') {
        if (-not (Test-Path -LiteralPath (Join-Path $compiler $name))) {
            throw "Missing compiler/$name. Extract the complete matching builder package before running the GUI."
        }
    }
    if (-not (Test-Path -LiteralPath $Xbe -PathType Leaf)) {
        throw 'Missing default.xbe. Select a valid extracted Xbox game folder or ISO in the builder GUI.'
    }
    $Xbe = (Resolve-Path -LiteralPath $Xbe).Path
    $expected = (Get-Content -LiteralPath (Join-Path $compiler 'xbe.sha1') -Raw).Trim()
    if ((Get-FileHash -LiteralPath $Xbe -Algorithm SHA1).Hash -ne $expected) {
        throw 'This default.xbe is not the version supported by this builder. Use the matching APK/builder and an unmodified game dump.'
    }
    if (-not $Jobs) {
        $memory = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory * 1KB
        # Leave one CPU thread available for the desktop and GUI.
        $Jobs = [int][Math]::Max(1, [Math]::Min(64, [Math]::Min([Environment]::ProcessorCount - 1, [Math]::Floor($memory / 512MB))))
    }
    $work = Join-Path $PSScriptRoot 'work'
    New-Item -ItemType Directory -Force -Path $work, (Join-Path $work 'generated'), (Join-Path $work 'obj') | Out-Null
    # An exclusive handle prevents two builders from writing the same sources and objects.
    $lock = [IO.File]::Open((Join-Path $work 'build.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    try {
        # A stable input path preserves Ninja state across fresh ISO extraction directories.
        $inputXbe = Join-Path $work 'default.xbe'
        if (-not (Test-Path -LiteralPath $inputXbe) -or (Get-FileHash -LiteralPath $inputXbe -Algorithm SHA1).Hash -ne $expected) {
            Copy-Item -LiteralPath $Xbe -Destination $inputXbe -Force
        }
        $Xbe = $inputXbe
        function NinjaPath([string]$path) {
            return $path.Replace('\', '/').Replace('$', '$$').Replace(':', '$:').Replace(' ', '$ ')
        }
        $sources = @('x86_func_mapping.cpp') + @(0..127 | ForEach-Object { "x86_recomp.$_.cpp" })
        $generated = @($sources | ForEach-Object { "generated/$_" }) + 'generated/x86_recomp_shared.h'
        $dependencies = @('../compiler/xbe_recomp.exe', '../compiler/config/scda_v2.toml', '../compiler/config/xdk_symbols.toml')
        $headers = @(Get-ChildItem -LiteralPath (Join-Path $compiler 'include') -Recurse -File | ForEach-Object {
            NinjaPath ('../compiler/include/' + $_.FullName.Substring((Join-Path $compiler 'include').Length + 1))
        })
        $lines = @(
            'ninja_required_version = 1.10',
            'rule translate',
            '  command = "../compiler/xbe_recomp.exe" "../compiler/config/scda_v2.toml" "$in_xbe" generated',
            '  description = Translating default.xbe',
            'rule compile',
            '  command = "../compiler/clang++.exe" --target=$triple -std=c++20 -O2 -fPIC -funwind-tables -fstack-protector-strong -fno-strict-aliasing $archflags -DX86_GAME_CODE -nostdinc -nostdinc++ -w -I../compiler/include -Igenerated -isystem ../compiler/include/freestanding -c $in -o $out',
            '  description = Compiling $in',
            'rule link',
            '  command = "../compiler/ld.lld.exe" -shared -soname libscda_code.so -z max-page-size=16384 -z now -z relro --hash-style=gnu --eh-frame-hdr --no-as-needed ../compiler/$abi/libscda_host.so @$out.rsp ../compiler/$abi/libunwind.a -o $out',
            '  rspfile = $out.rsp',
            '  rspfile_content = $in',
            '  description = Linking libscda_code.so',
            "build $($generated -join ' '): translate $(NinjaPath $Xbe) | $($dependencies -join ' ')",
            ('  in_xbe = ' + $Xbe.Replace('\', '/').Replace('$', '$$'))
        )
        $outputs = @()
        foreach ($abi in 'arm64-v8a', 'x86_64') {
            $triple = if ($abi -eq 'arm64-v8a') { 'aarch64-linux-android29' } else { 'x86_64-linux-android29' }
            $archflags = if ($abi -eq 'arm64-v8a') { '-mno-outline-atomics' } else { '' }
            New-Item -ItemType Directory -Force -Path "$work/obj/$abi", "$work/lib/$abi" | Out-Null
            $objects = @($sources | ForEach-Object { "obj/$abi/$_.o" })
            for ($i = 0; $i -lt $sources.Count; $i++) {
                $lines += "build $($objects[$i]): compile generated/$($sources[$i]) | generated/x86_recomp_shared.h ../compiler/clang++.exe $($headers -join ' ')"
                $lines += "  triple = $triple", "  archflags = $archflags"
            }
            $output = "lib/$abi/libscda_code.so"
            $outputs += $output
            $lines += "build ${output}: link $($objects -join ' ') | ../compiler/ld.lld.exe ../compiler/$abi/libscda_host.so ../compiler/$abi/libunwind.a"
            $lines += "  abi = $abi"
        }
        [IO.File]::WriteAllLines((Join-Path $work 'build.ninja'), $lines, [Text.UTF8Encoding]::new($false))
        Push-Location $work
        try {
            & '../compiler/ninja.exe' -j $Jobs @outputs
            if ($LASTEXITCODE) { throw 'Build failed. Fix the error above and run again; completed objects are kept.' }
        } finally { Pop-Location }
        foreach ($abi in 'arm64-v8a', 'x86_64') {
            New-Item -ItemType Directory -Force -Path "$PSScriptRoot/lib/$abi" | Out-Null
            Copy-Item -LiteralPath "$work/lib/$abi/libscda_code.so" -Destination "$PSScriptRoot/lib/$abi/libscda_code.so" -Force
        }
    } finally { $lock.Dispose() }
    Write-Host "`nReady: $(Join-Path $PSScriptRoot 'lib') (ARM64 + x86_64)"
    Write-Host 'Game-code compilation complete.'
} catch {
    Write-Host "`nERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
