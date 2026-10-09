param(
    [Parameter(Mandatory=$true)][string]$YmirSource,
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$DxcExecutable,
    [string]$InstalledDependencies
)
$ErrorActionPreference = 'Stop'
$integrationSource = (Resolve-Path -LiteralPath $YmirSource).Path
$integrationComponents = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../components/xband')).Path
$integrationDxc = (Resolve-Path -LiteralPath $DxcExecutable).Path
$integrationPatch = Join-Path $PSScriptRoot 'ymir-9a237ea-serial-xband.patch'
& git -C $integrationSource apply --reverse --check $integrationPatch
if ($LASTEXITCODE -ne 0) { throw 'Apply the matching integration patch before building.' }
$integrationToolchain = Join-Path $integrationSource 'vcpkg/scripts/buildsystems/vcpkg.cmake'
if (-not (Test-Path -LiteralPath $integrationToolchain)) { throw 'Initialize YMIR submodules first.' }
foreach ($integrationDll in @('dxcompiler.dll','dxil.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $integrationDxc) $integrationDll))) {
        throw "DXC runtime DLL missing: $integrationDll"
    }
}
$integrationArguments = @(
    '-S', $integrationSource, '-B', $BuildDirectory, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DCMAKE_TOOLCHAIN_FILE=$integrationToolchain", "-DDXC_EXECUTABLE=$integrationDxc",
    "-DYmir_XBAND_COMPONENTS_DIR=$integrationComponents",
    '-DYmir_ENABLE_IPO=OFF', '-DYmir_AVX2=ON', '-DYmir_ENABLE_DEVLOG=OFF',
    '-DYmir_ENABLE_TESTS=ON', '-DYmir_ENABLE_SANDBOX=OFF', '-DYmir_ENABLE_YMDASM=OFF',
    '-DYmir_EXTRA_INLINING=OFF'
)
if ($InstalledDependencies) {
    $integrationDependencies = (Resolve-Path -LiteralPath $InstalledDependencies).Path
    $integrationArguments += @("-DVCPKG_INSTALLED_DIR=$integrationDependencies", '-DVCPKG_MANIFEST_INSTALL=OFF')
} else {
    $integrationArguments += '-DVCPKG_MANIFEST_INSTALL=ON'
}
& cmake @integrationArguments
if ($LASTEXITCODE -ne 0) { throw 'YMIR CMake configuration failed.' }
& cmake --build $BuildDirectory --config Release --parallel 3
if ($LASTEXITCODE -ne 0) { throw 'YMIR build failed.' }
$integrationAppDirectory = Join-Path $BuildDirectory 'apps/ymir-sdl3/Release'
foreach ($integrationDll in @('dxcompiler.dll','dxil.dll')) {
    Copy-Item -LiteralPath (Join-Path (Split-Path $integrationDxc) $integrationDll) -Destination (Join-Path $integrationAppDirectory $integrationDll)
}
& ctest --test-dir $BuildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'YMIR tests failed.' }
Write-Output ('PASS local YMIR build: ' + (Join-Path $integrationAppDirectory 'ymir-sdl3.exe'))
Write-Output 'Not included in the server distribution. No game or server was started.'
