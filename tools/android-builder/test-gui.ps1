$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = Join-Path $repo 'out/gui-responsiveness-test'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
if (-not (Test-Path $csc)) { $csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework/v4.0.30319/csc.exe' }
$wpf = Join-Path (Split-Path $csc) 'WPF'
$source = (Resolve-Path (Join-Path $PSScriptRoot 'Builder.cs')).Path
$test = (Resolve-Path (Join-Path $PSScriptRoot 'GuiTest.cs')).Path
$exe = Join-Path $out 'GuiTest.exe'
& $csc /nologo /target:exe /main:GuiTest /optimize+ /out:$exe `
    /reference:System.dll /reference:System.Core.dll /reference:System.Xaml.dll `
    /reference:"$wpf/WindowsBase.dll" /reference:"$wpf/PresentationCore.dll" /reference:"$wpf/PresentationFramework.dll" $source $test
if ($LASTEXITCODE) { throw 'GUI test compilation failed.' }
& $exe (Join-Path $out 'preview.png')
if ($LASTEXITCODE) { throw 'GUI responsiveness test failed.' }
