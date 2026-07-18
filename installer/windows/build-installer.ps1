param(
    [string]$Version = "4.2.dev0",
    [string]$QtBin = "C:\Qt\6.8.3\mingw_64\bin",
    [string]$InnoCompiler = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$dist = Join-Path $projectRoot "dist"
$portable = Join-Path $dist "Seed-Atlas-$Version-Windows-x64-Portable"
$sourceArchive = Join-Path $dist "Seed-Atlas-$Version-Source.zip"
$sourceStage = Join-Path $dist "source-stage"
$sourceFolder = Join-Path $sourceStage "Seed-Atlas-$Version-Source"
$deployTool = Join-Path $QtBin "windeployqt.exe"
$application = Join-Path $projectRoot "seed-atlas.exe"

if (!(Test-Path -LiteralPath $application)) { throw "Missing application: $application" }
if (!(Test-Path -LiteralPath $deployTool)) { throw "Missing windeployqt: $deployTool" }
if (!(Test-Path -LiteralPath $InnoCompiler)) { throw "Missing Inno Setup compiler: $InnoCompiler" }

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

New-Item -ItemType Directory -Path $sourceFolder -Force | Out-Null
foreach ($directory in @("src", "seedatlas-engine", "lua", "rc", "etc", "installer")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $directory) -Destination $sourceFolder -Recurse
}
foreach ($file in @("seed-atlas.pro", "buildguide.md", "README.md", "LICENSE", "LEGAL_NOTICE.md", "THIRD_PARTY_NOTICES.md", "SOURCE_CODE.md", ".gitignore")) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $file) -Destination $sourceFolder
}
Get-ChildItem -LiteralPath (Join-Path $sourceFolder "seedatlas-engine") -File |
    Where-Object { $_.Extension -in @(".o", ".a", ".exe") } |
    Remove-Item -Force
Compress-Archive -Path $sourceFolder -DestinationPath $sourceArchive -CompressionLevel Optimal
Remove-Item -LiteralPath $sourceStage -Recurse -Force

& $InnoCompiler "/DMyAppVersion=$Version" (Join-Path $PSScriptRoot "SeedAtlas.iss")
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }

Write-Host "Created:"
Write-Host "  $portable"
Write-Host "  $sourceArchive"
Write-Host "  $(Join-Path $dist "Seed-Atlas-$Version-Windows-x64-Setup.exe")"
