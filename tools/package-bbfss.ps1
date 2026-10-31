param(
    [Parameter(Mandatory)][string]$Destination,
    [string]$RuntimeBin = 'C:\msys64\clang64\bin'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$destination = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $destination) {
    if (@(Get-ChildItem -LiteralPath $destination -Force).Count) {
        throw "Destination must be empty: $destination"
    }
}
$bin = Join-Path $destination 'engine\out'
New-Item -ItemType Directory -Force -Path $bin | Out-Null
Copy-Item -LiteralPath "$root\engine\out\bbfss-launcher\BBFSS.exe" -Destination $destination
foreach ($name in 'bb-probe.exe', 'bb-start.exe', 'bb-engine-assets.exe', 'bb-gpu-capabilities.exe', 'bb-shader-lab.exe') {
    Copy-Item -LiteralPath "$root\engine\out\$name" -Destination $bin
}
foreach ($name in 'patches', 'data', 'licenses') {
    Copy-Item -LiteralPath "$root\engine\$name" -Destination "$destination\engine" -Recurse
}
foreach ($name in 'gpu-cache.id', 'gpu-cache.legacy.id', 'bloodborne.ico', 'nvngx_dlss.dll', 'LICENSE-DLSS-SDK.txt') {
    if (Test-Path -LiteralPath "$root\engine\out\$name") {
        Copy-Item -LiteralPath "$root\engine\out\$name" -Destination $bin
    }
}
foreach ($name in 'fsr4_shaders', 'fsr4_411', 'fsr4vk') {
    if (Test-Path -LiteralPath "$root\engine\$name") {
        Copy-Item -LiteralPath "$root\engine\$name" -Destination "$destination\engine" -Recurse
    }
}
foreach ($name in 'LICENSE', 'NOTICE', 'README.md') {
    Copy-Item -LiteralPath "$root\$name" -Destination $destination
}
Copy-Item -LiteralPath "$PSScriptRoot\bbfss-default.ini" -Destination "$destination\engine\lantern.default.ini"
Copy-Item -LiteralPath "$PSScriptRoot\bbfss-default.ini" -Destination "$destination\lantern.ini"
Copy-Item -LiteralPath "$PSScriptRoot\BBFSS-README.txt" -Destination "$destination\README-FIRST.txt"
$commit = (& git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE) { throw 'Cannot identify source revision' }
@{ version = '0.9.9'; engine = "bbfss-$($commit.Substring(0,7))"; source_commit = $commit;
    source_modified = @(& git -C $root status --porcelain --untracked-files=no).Count -gt 0 } |
    ConvertTo-Json | Set-Content -LiteralPath "$destination\package.json" -Encoding utf8

# Copy the transitive non-system imports, rather than relying on the developer's PATH.
$queue = [Collections.Generic.Queue[string]]::new()
Get-ChildItem -LiteralPath $bin -File | Where-Object { $_.Extension -in '.exe', '.dll' } |
    ForEach-Object { $queue.Enqueue($_.FullName) }
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($queue.Count) {
    $file = $queue.Dequeue()
    if (!$seen.Add([IO.Path]::GetFileName($file))) { continue }
    $imports = & "$RuntimeBin\llvm-readobj.exe" --coff-imports $file
    if ($LASTEXITCODE) { throw "Cannot inspect imports: $file" }
    foreach ($line in $imports) {
        if ($line -notmatch '^\s+Name: (.+\.dll)\s*$') { continue }
        $name = $Matches[1]
        if ($name -match '^(api-ms-|ext-ms-)') { continue }
        if (Test-Path -LiteralPath "$bin\$name") {
            $queue.Enqueue("$bin\$name")
        } elseif ($name -ine 'vulkan-1.dll' -and (Test-Path -LiteralPath "$RuntimeBin\$name")) {
            Copy-Item -LiteralPath "$RuntimeBin\$name" -Destination $bin
            $queue.Enqueue("$bin\$name")
        } elseif (!(Test-Path -LiteralPath "$env:SystemRoot\System32\$name")) {
            throw "Unresolved runtime dependency $name imported by $file"
        }
    }
}
# The MSYS2 package notices include FFmpeg's configuration-specific license terms.
$licenseRoot = Join-Path $destination 'engine\licenses\runtime'
New-Item -ItemType Directory -Force -Path $licenseRoot | Out-Null
Copy-Item -LiteralPath (Join-Path (Split-Path $RuntimeBin -Parent) 'share\licenses') `
    -Destination "$licenseRoot\msys2" -Recurse

# Include corresponding checked-out source, never game-derived or ignored runtime files.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$source = [IO.Compression.ZipFile]::Open("$destination\BBFSS-source.zip", [IO.Compression.ZipArchiveMode]::Create)
try {
    $paths = @(& git -C $root ls-files --recurse-submodules)
    if ($LASTEXITCODE) { throw 'Cannot enumerate source files' }
    $paths += @('tools/package-bbfss.ps1', 'tools/bbfss-default.ini', 'tools/BBFSS-README.txt',
        'engine/start/performance.inc', 'engine/tests/test_point_volumetric.cpp',
        'engine/tests/test_builtin_performance.py',
        'engine/tests/test_motion_blur.cpp',
        'engine/tests/test_exposure_policy.cpp',
        'engine/tests/test_native_exposure.cpp',
        'engine/gpu/shim/engine/exposure_policy.h',
        'engine/gpu/shim/engine/auto_exposure.h',
        'engine/gpu/shim/engine/auto_exposure.cpp',
        'engine/tests/test_volumetric_tilt.cpp',
        'engine/gpu/shadps4/video_core/host_shaders/volumetric_tilt.glsl',
        'engine/gpu/shadps4/video_core/host_shaders/volumetric_tilt_test.comp',
        'engine/gpu/shadps4/video_core/host_shaders/native_motion_blur.glsl',
        'engine/gpu/shadps4/video_core/host_shaders/native_motion_blur.frag',
        'engine/gpu/shadps4/video_core/host_shaders/native_motion_blur_test.comp')
    foreach ($relative in ($paths | Sort-Object -Unique)) {
        $local = Join-Path $root ($relative.Replace('/', '\'))
        if (Test-Path -LiteralPath $local -PathType Leaf) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $source, $local, $relative, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
} finally { $source.Dispose() }
Get-ChildItem -LiteralPath $destination -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash,
            [IO.Path]::GetRelativePath($destination, $_.FullName)
    } | Set-Content -LiteralPath "$destination\SHA256SUMS.txt" -Encoding ascii
Write-Output "Packaged BBFSS: $destination"
