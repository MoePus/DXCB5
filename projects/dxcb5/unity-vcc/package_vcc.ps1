param(
    [string]$BuildDir = "F:\.codex-tmp\dxcb5-x64",
    [string]$OutDir = "F:\.codex-tmp\dxcb5-vcc-dist",
    [string]$RepoUrl = "https://moepus.github.io/dxcb5-vcc",
    [string]$DownloadBaseUrl = "https://github.com/MoePus/dxcb5/releases/download"
)

$ErrorActionPreference = "Stop"

$pkgSrc = Join-Path $PSScriptRoot "com.moepus.dxcb5"
$manifestPath = Join-Path $pkgSrc "package.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$pkgName = $manifest.name
$version = $manifest.version

$proxyDll = Join-Path $BuildDir "bin\unity-proxy\D3DCompiler_47.dll"
$dxcDll = Join-Path $BuildDir "bin\dxcompiler.dll"

if (-not (Test-Path -LiteralPath $proxyDll)) {
    throw "Missing built proxy DLL: $proxyDll (build target DXCB5UnityProxy first)"
}
if (-not (Test-Path -LiteralPath $dxcDll)) {
    throw "Missing built dxcompiler DLL: $dxcDll (build target dxcompiler first)"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stageDir = Join-Path $OutDir "_stage"
if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
Copy-Item -LiteralPath $pkgSrc -Destination $stageDir -Recurse -Force

$nativeParent = Join-Path $stageDir "Editor\Native"
$nativeDir = Join-Path $nativeParent "win-x64"
New-Item -ItemType Directory -Force -Path $nativeDir | Out-Null

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$folderMeta1 = @"
fileFormatVersion: 2
guid: d5c8b50147a04b219f30000000000020
folderAsset: yes
DefaultImporter:
  externalObjects: {}
  userData: 
  assetBundleName: 
  assetBundleVariant: 
"@
$folderMeta2 = @"
fileFormatVersion: 2
guid: d5c8b50147a04b219f30000000000021
folderAsset: yes
DefaultImporter:
  externalObjects: {}
  userData: 
  assetBundleName: 
  assetBundleVariant: 
"@
[System.IO.File]::WriteAllText("$nativeParent.meta", $folderMeta1, $utf8NoBom)
[System.IO.File]::WriteAllText("$nativeDir.meta", $folderMeta2, $utf8NoBom)

$dstProxyBin = Join-Path $nativeDir "D3DCompiler_47.dll.bin"
$dstDxcBin = Join-Path $nativeDir "dxcb5_dxcompiler.dll.bin"
Copy-Item -LiteralPath $proxyDll -Destination $dstProxyBin -Force
Copy-Item -LiteralPath $dxcDll -Destination $dstDxcBin -Force

# Deterministic DefaultAsset .meta files so Unity never auto-loads these binaries into Unity.exe
$proxyMeta = @"
fileFormatVersion: 2
guid: d5c8b50147a04b219f30000000000101
DefaultImporter:
  externalObjects: {}
  userData: 
  assetBundleName: 
  assetBundleVariant: 
"@
$dxcMeta = @"
fileFormatVersion: 2
guid: d5c8b50147a04b219f30000000000102
DefaultImporter:
  externalObjects: {}
  userData: 
  assetBundleName: 
  assetBundleVariant: 
"@
[System.IO.File]::WriteAllText("$dstProxyBin.meta", $proxyMeta, $utf8NoBom)
[System.IO.File]::WriteAllText("$dstDxcBin.meta", $dxcMeta, $utf8NoBom)

$zipName = "$pkgName-$version.zip"
$zipPath = Join-Path $OutDir $zipName
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $stageDir,
    $zipPath,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $false
)

$sha256 = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$downloadUrl = "$DownloadBaseUrl/v$version/$zipName"

$versionEntry = [ordered]@{}
foreach ($prop in $manifest.PSObject.Properties) {
    $versionEntry[$prop.Name] = $prop.Value
}
$versionEntry["url"] = $downloadUrl
$versionEntry["zipSHA256"] = $sha256

$vpmIndex = [ordered]@{
    name = "DXCB5 Shader Compiler VCC Repository"
    id = "com.moepus.dxcb5.repo"
    url = "$RepoUrl/vpm-index.json"
    author = "MoePus"
    packages = [ordered]@{
        $pkgName = [ordered]@{
            versions = [ordered]@{
                $version = $versionEntry
            }
        }
    }
}

$indexPath = Join-Path $OutDir "vpm-index.json"
$indexJson = $vpmIndex | ConvertTo-Json -Depth 10
[System.IO.File]::WriteAllText($indexPath, $indexJson, (New-Object System.Text.UTF8Encoding($false)))

Remove-Item -LiteralPath $stageDir -Recurse -Force

$zipSizeMB = [math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 2)
Write-Host "Built VCC package: $zipPath ($zipSizeMB MB)"
Write-Host "SHA256: $sha256"
Write-Host "Built VPM index:  $indexPath"
