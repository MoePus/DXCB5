param(
    [string]$BuildDir = "",
    [string]$OutDir = "",
    [string]$RepoUrl = "https://moepus.github.io/vpm",
    [string]$DownloadBaseUrl = "https://github.com/MoePus/DXCB5/releases/download",
    [string]$Version = "",
    [string]$TagName = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build"
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $repoRoot "dist"
}

$pkgSrc = Join-Path $PSScriptRoot "com.moepus.dxcb5"
$manifestPath = Join-Path $pkgSrc "package.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$pkgName = $manifest.name
$effectiveVersion = if ([string]::IsNullOrWhiteSpace($Version)) { $manifest.version } else { $Version.Trim() }
$effectiveTag = if ([string]::IsNullOrWhiteSpace($TagName)) { "v$effectiveVersion" } else { $TagName.Trim() }

$dxcDll = Join-Path $BuildDir "bin\dxcompiler.dll"
$unityHookDll = Join-Path $BuildDir "bin\unity-hook\dxcb5_unity_hook.dll"
$cgBatchPluginDll = Join-Path $BuildDir "bin\unity-hook\dxcb5_cgbatch_plugin.dll"

if (-not (Test-Path -LiteralPath $dxcDll)) {
    throw "Missing built dxcompiler DLL: $dxcDll (build target dxcompiler first)"
}
if (-not (Test-Path -LiteralPath $unityHookDll)) {
    throw "Missing built unity hook DLL: $unityHookDll (build target DXCB5UnityHook first)"
}
if (-not (Test-Path -LiteralPath $cgBatchPluginDll)) {
    throw "Missing built CgBatch plugin DLL: $cgBatchPluginDll (build target DXCB5CgBatchPlugin first)"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stageDir = Join-Path $OutDir "_stage"
if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
Copy-Item -LiteralPath $pkgSrc -Destination $stageDir -Recurse -Force

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# Apply effective version override to staged package.json (without mutating repo)
if ($effectiveVersion -ne $manifest.version) {
    $stagedManifestPath = Join-Path $stageDir "package.json"
    $manifest.version = $effectiveVersion
    $stagedJson = $manifest | ConvertTo-Json -Depth 10
    [System.IO.File]::WriteAllText($stagedManifestPath, $stagedJson, $utf8NoBom)
}

$nativeParent = Join-Path $stageDir "Editor\Native"
$nativeDir = Join-Path $nativeParent "win-x64"
New-Item -ItemType Directory -Force -Path $nativeDir | Out-Null

function Write-DefaultMeta([string]$Path, [string]$Guid, [switch]$Folder) {
    $folderLine = if ($Folder) { "folderAsset: yes`n" } else { "" }
    $meta = @"
fileFormatVersion: 2
guid: $Guid
${folderLine}DefaultImporter:
  externalObjects: {}
  userData:
  assetBundleName:
  assetBundleVariant:
"@
    [System.IO.File]::WriteAllText($Path, $meta, $utf8NoBom)
}

Write-DefaultMeta "$nativeParent.meta" "d5c8b50147a04b219f30000000000020" -Folder
Write-DefaultMeta "$nativeDir.meta" "d5c8b50147a04b219f30000000000021" -Folder

$dstDxcBin = Join-Path $nativeDir "dxcb5_dxcompiler.dll.bin"
$dstCgBatchBin = Join-Path $nativeDir "dxcb5_cgbatch_plugin.dll.bin"
$dstUnityHookDll = Join-Path $nativeDir "dxcb5_unity_hook.dll"
Copy-Item -LiteralPath $dxcDll -Destination $dstDxcBin -Force
Copy-Item -LiteralPath $cgBatchPluginDll -Destination $dstCgBatchBin -Force
Copy-Item -LiteralPath $unityHookDll -Destination $dstUnityHookDll -Force

# Deterministic DefaultAsset .meta files so Unity never auto-loads worker binaries into Unity.exe,
# plus Editor-only x64 PluginImporter .meta for dxcb5_unity_hook.dll.
Write-DefaultMeta "$dstDxcBin.meta" "d5c8b50147a04b219f30000000000102"
Write-DefaultMeta "$dstCgBatchBin.meta" "d5c8b50147a04b219f30000000000103"
$unityHookMeta = @"
fileFormatVersion: 2
guid: d5c8b50147a04b219f30000000000104
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any:
    second:
      enabled: 0
      settings: {}
  - first:
      Editor: Editor
    second:
      enabled: 1
      settings:
        CPU: x86_64
        DefaultValueInitialized: true
        OS: Windows
  userData:
  assetBundleName:
  assetBundleVariant:
"@
[System.IO.File]::WriteAllText("$dstUnityHookDll.meta", $unityHookMeta, $utf8NoBom)

$zipName = "$pkgName-$effectiveVersion.zip"
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
$downloadUrl = "$DownloadBaseUrl/$effectiveTag/$zipName"

$versionEntry = [ordered]@{}
foreach ($prop in $manifest.PSObject.Properties) {
    $versionEntry[$prop.Name] = $prop.Value
}
$versionEntry["version"] = $effectiveVersion
$versionEntry["url"] = $downloadUrl
$versionEntry["zipSHA256"] = $sha256

# Emit standalone release package.json (with url + zipSHA256) and SHA256SUMS.txt for VPM indexers
$releasePkgPath = Join-Path $OutDir "package.json"
[System.IO.File]::WriteAllText($releasePkgPath, ($versionEntry | ConvertTo-Json -Depth 10), $utf8NoBom)

$shaFilePath = Join-Path $OutDir "SHA256SUMS.txt"
[System.IO.File]::WriteAllText($shaFilePath, "$sha256  $zipName`n", $utf8NoBom)

$vpmIndex = [ordered]@{
    name = "MoePus VPM Repository"
    id = "com.moepus.vpm"
    url = "$RepoUrl/index.json"
    author = "MoePus"
    packages = [ordered]@{
        $pkgName = [ordered]@{
            versions = [ordered]@{
                $effectiveVersion = $versionEntry
            }
        }
    }
}

$indexPath = Join-Path $OutDir "vpm-index.json"
[System.IO.File]::WriteAllText($indexPath, ($vpmIndex | ConvertTo-Json -Depth 10), $utf8NoBom)

Remove-Item -LiteralPath $stageDir -Recurse -Force

$zipSizeMB = [math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 2)
Write-Host "Built VCC package: $zipPath ($zipSizeMB MB)"
Write-Host "Version: $effectiveVersion (tag: $effectiveTag)"
Write-Host "SHA256:  $sha256"
Write-Host "Outputs: $releasePkgPath, $shaFilePath, $indexPath"
