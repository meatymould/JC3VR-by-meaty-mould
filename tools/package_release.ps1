param(
    [string]$Version = '0.1.0-experimental',
    [string]$BuildDir = (Join-Path $PSScriptRoot '../build')
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^[a-zA-Z0-9][a-zA-Z0-9._-]*$') { throw 'Invalid package version.' }
$root = Split-Path $PSScriptRoot -Parent
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$ctest = Get-Command ctest -ErrorAction Stop
& $ctest.Source --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed. Package creation stopped.' }
$output = $build
if (Test-Path -LiteralPath (Join-Path $build 'Release/jc3vr.dll')) { $output = Join-Path $build 'Release' }
foreach ($name in @('jc3vr.dll', 'dinput8.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $output $name))) { throw "Missing $name" }
}
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist "JC3VR-$Version"
$zip = "$stage.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) {
    throw 'Package already exists. Choose a fresh version or move it aside.'
}
New-Item -ItemType Directory -Path (Join-Path $stage 'game/mods') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $output 'jc3vr.dll') -Destination (Join-Path $stage 'game/mods/jc3vr.dll')
Copy-Item -LiteralPath (Join-Path $output 'dinput8.dll') -Destination (Join-Path $stage 'game/dinput8.dll')
Copy-Item -LiteralPath (Join-Path $root 'packaging/jc3vr.ini') -Destination (Join-Path $stage 'game/jc3vr.ini')
foreach ($name in @('README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'requirements-dev.txt')) {
    Copy-Item -LiteralPath (Join-Path $root $name) -Destination $stage
}
# Ship licenses and local preparation tools, never the original game artwork.
New-Item -ItemType Directory -Path (Join-Path $stage 'third_party') -Force | Out-Null
foreach ($directory in @('docs','tools','third_party/licenses')) {
    Copy-Item -LiteralPath (Join-Path $root $directory) -Destination (Join-Path $stage $directory) -Recurse
}
New-Item -ItemType Directory -Path (Join-Path $stage 'third_party/minhook') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage 'third_party/openxr') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'third_party/minhook/LICENSE.txt') -Destination (Join-Path $stage 'third_party/minhook/LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'third_party/openxr/LICENSE') -Destination (Join-Path $stage 'third_party/openxr/LICENSE')
# Exclude local Python caches from the package's copies.
Get-ChildItem -LiteralPath (Join-Path $stage 'tools') -Recurse -File -Filter '*.pyc' | ForEach-Object {
    Remove-Item -LiteralPath $_.FullName
}
$assetExtensions = @('.rbm','.ee','.arc','.tab','.dds','.ddsc','.hmddsc','.pfxc','.ban','.gfx','.rgba','.png','.jpg','.mp4','.mov','.pdb')
$unexpected = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in $assetExtensions }
if ($unexpected) { throw 'Unexpected game assets, media or debug files in package.' }
$sums = Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),
        $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
}
Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Value $sums -Encoding ascii
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Output "Packaged $zip"
Get-FileHash -LiteralPath $zip -Algorithm SHA256
