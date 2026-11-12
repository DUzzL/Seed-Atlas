param(
    [string]$Version = "",
    [string]$QtBin = "C:\Qt\6.8.3\mingw_64\bin",
    [string]$MingwBin = "C:\Qt\Tools\mingw1310_64\bin",
    [string]$InnoCompiler = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if ([string]::IsNullOrWhiteSpace($Version)) {
    # the one source of the version, as for the app build (seed-atlas.pro)
    $Version = (Get-Content -LiteralPath (Join-Path $projectRoot "VERSION.txt") -Raw).Trim()
}
$versionMatch = [regex]::Match($Version, "^(\d+)\.(\d+)\.(dev)?(\d+)$")
if (!$versionMatch.Success) { throw "Invalid Seed Atlas version: $Version" }
# Windows file versions are numeric: development versions count as patch 0
$g = $versionMatch.Groups
$numericVersion = if ($g[3].Success) { "$($g[1].Value).$($g[2].Value).0.0" } else { "$($g[1].Value).$($g[2].Value).$($g[4].Value).0" }
$dist = Join-Path $projectRoot "dist"
$portable = Join-Path $dist "Seed-Atlas-$Version-Windows-x64-Portable"
$sourceArchive = Join-Path $dist "Seed-Atlas-$Version-Source.zip"
$sourceStage = Join-Path $dist "source-stage"
$sourceFolder = Join-Path $sourceStage "Seed-Atlas-$Version-Source"
$build = Join-Path $projectRoot "build-windows"
$deployTool = Join-Path $QtBin "windeployqt.exe"
$qmake = Join-Path $QtBin "qmake.exe"
$make = Join-Path $MingwBin "mingw32-make.exe"
$application = Join-Path $build "release\seed-atlas.exe"
$buildId = $env:SEED_ATLAS_BUILD_ID
if ([string]::IsNullOrWhiteSpace($buildId)) {
    $buildId = (& git -C $projectRoot rev-parse --verify HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw "Could not determine the Git commit" }
}
if ($buildId -notmatch "^[0-9a-fA-F]{40}$") {
    throw "Could not determine the Git commit used as the Seed Atlas build ID"
}
$buildId = $buildId.ToLowerInvariant()
# commit time in seconds since 1970; the update check orders builds by it
$buildTime = $env:SEED_ATLAS_BUILD_TIME
if ([string]::IsNullOrWhiteSpace($buildTime)) {
    $buildTime = (& git -C $projectRoot show -s --format=%ct $buildId).Trim()
    if ($LASTEXITCODE -ne 0) { throw "Could not determine the commit time" }
}
if ($buildTime -notmatch "^[0-9]+$") {
    throw "Could not determine the commit time used as the Seed Atlas build time"
}

if (!(Test-Path -LiteralPath $deployTool)) { throw "Missing windeployqt: $deployTool" }
if (!(Test-Path -LiteralPath $qmake)) { throw "Missing qmake: $qmake" }
if (!(Test-Path -LiteralPath $make)) { throw "Missing MinGW make: $make" }
if (!(Test-Path -LiteralPath $InnoCompiler)) { throw "Missing Inno Setup compiler: $InnoCompiler" }

New-Item -ItemType Directory -Path $build -Force | Out-Null
$oldPath = $env:PATH
try {
    $env:PATH = "$MingwBin;$QtBin;$oldPath"
    Push-Location $build
    # Engine objects live in the source tree; drop any left over from
    # development builds so the release links freshly built ones.
    Get-ChildItem -LiteralPath (Join-Path $projectRoot "seedatlas-engine") -File |
        Where-Object { $_.Extension -in @(".o", ".a", ".d") } |
        Remove-Item -Force
    & $qmake -o Makefile "CONFIG+=release" "SEED_ATLAS_BUILD_ID=$buildId" "SEED_ATLAS_BUILD_TIME=$buildTime" (Join-Path $projectRoot "seed-atlas.pro")
    if ($LASTEXITCODE -ne 0) { throw "qmake failed with exit code $LASTEXITCODE" }
    & $make "-j$([Environment]::ProcessorCount)"
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
}
finally {
    Pop-Location
    $env:PATH = $oldPath
}
if (!(Test-Path -LiteralPath $application)) { throw "Build did not create: $application" }

New-Item -ItemType Directory -Path $dist -Force | Out-Null
foreach ($target in @($portable, $sourceArchive, $sourceStage)) {
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
}
New-Item -ItemType Directory -Path $portable -Force | Out-Null
Copy-Item -LiteralPath $application -Destination $portable
& $deployTool --release --compiler-runtime --no-translations --dir $portable (Join-Path $portable "seed-atlas.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

foreach ($file in @("LICENSE", "LEGAL_NOTICE.md", "THIRD_PARTY_NOTICES.md", "README.md", "SOURCE_CODE.md")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $portable
}
# license texts of the bundled libraries (Qt: LGPLv3) and of the engine
$licenses = Join-Path $portable "Licenses"
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot "LICENSE") -Destination (Join-Path $licenses "LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $projectRoot "installer\licenses\LGPL-3.0.txt") -Destination $licenses
Copy-Item -LiteralPath (Join-Path $projectRoot "seedatlas-engine\LICENSE") -Destination (Join-Path $licenses "cubiomes-LICENSE.txt")

New-Item -ItemType Directory -Path $sourceFolder -Force | Out-Null
foreach ($directory in @(".github", "src", "seedatlas-engine", "seedatlas-sax", "lua", "rc", "etc", "installer", "tools")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $directory) -Destination $sourceFolder -Recurse
}
foreach ($file in @("seed-atlas.pro", "VERSION.txt", "buildguide.md", "README.md", "LICENSE", "LEGAL_NOTICE.md", "THIRD_PARTY_NOTICES.md", "SOURCE_CODE.md", ".gitignore")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $sourceFolder
}
Get-ChildItem -LiteralPath (Join-Path $sourceFolder "seedatlas-engine") -File |
    Where-Object { $_.Extension -in @(".o", ".a", ".d", ".exe") } |
    Remove-Item -Force
# build products of the mod library and of the tools
$saxBuild = Join-Path $sourceFolder "seedatlas-sax\build"
if (Test-Path -LiteralPath $saxBuild) { Remove-Item -LiteralPath $saxBuild -Recurse -Force }
Get-ChildItem -LiteralPath $sourceFolder -Recurse -File |
    Where-Object { $_.Extension -in @(".o", ".a", ".d", ".exe") } |
    Remove-Item -Force
Compress-Archive -Path $sourceFolder -DestinationPath $sourceArchive -CompressionLevel Optimal
Remove-Item -LiteralPath $sourceStage -Recurse -Force

& $InnoCompiler "/DMyAppVersion=$Version" "/DMyAppNumericVersion=$numericVersion" (Join-Path $PSScriptRoot "SeedAtlas.iss")
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }

$updateManifest = Join-Path $dist "update.json"
# "buildId" is all that builds before 4.3 read; newer builds also use
# "buildTime" to offer only newer releases.
$updateJson = "{`n  `"buildId`": `"$buildId`",`n  `"buildTime`": $buildTime,`n  `"version`": `"$Version`"`n}`n"
[System.IO.File]::WriteAllText(
    $updateManifest,
    $updateJson,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host "Created:"
Write-Host "  $portable"
Write-Host "  $sourceArchive"
Write-Host "  $(Join-Path $dist "Seed-Atlas-$Version-Windows-x64-Setup.exe")"
Write-Host "  $updateManifest"
