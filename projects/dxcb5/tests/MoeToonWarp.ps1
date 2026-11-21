# Distributed under the University of Illinois Open Source License.
# The sources are the raw HLSLPROGRAM contents from the Plain and Body ForwardBase passes.
param(
  [Parameter(Mandatory)][string] $DXCB5,
  [string] $FXC,
  [string] $WarpTest,
  [string] $DXC,
  [Parameter(Mandatory)][string] $UnityIncludes,
  [Parameter(Mandatory)][string] $PlainHlsl,
  [Parameter(Mandatory)][string] $BodyHlsl,
  [Parameter(Mandatory)][string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
if (!$DXC -and (!$FXC -or !$WarpTest)) {
  throw 'Supply -DXC to compare the optimized IR, or supply -FXC and -WarpTest to compare draws.'
}

$common = @(
  'SHADER_API_D3D11=1'
  'SHADER_API_DESKTOP=1'
  'SHADER_TARGET=50'
  'UNITY_VERSION=2022322'
  'UNITY_PASS_FORWARDBASE=1'
  'UNITY_PBS_USE_BRDF1=1'
  'UNITY_LIGHT_PROBE_PROXY_VOLUME=1'
  'UNITY_LIGHTMAP_FULL_HDR=1'
  'UNITY_ENABLE_REFLECTION_BUFFERS=1'
  'DIRECTIONAL=1'
  'LIGHTPROBE_SH=1'
)
$plain = @(
  'UNITY_USE_DITHER_MASK_FOR_ALPHABLENDED_SHADOWS=1'
  'UNITY_SPECCUBE_BOX_PROJECTION=1'
  'UNITY_SPECCUBE_BLENDING=1'
  'UNITY_ENABLE_DETAIL_NORMALMAP=1'
)
$stereo = @('STEREO_INSTANCING_ON=1', 'INSTANCING_ON=1', 'FOG_LINEAR=1')
$body = @('SHADOWS_SCREEN=1')
$body += @(Get-Content -LiteralPath $BodyHlsl -Encoding UTF8 | ForEach-Object {
  if ($_ -match '^#pragma shader_feature_local _ (MT2F_(?!NO_SHADOW_CASTER)\w+)') {
    "$($Matches[1])=1"
  }
})

function Invoke-Checked($Executable, $Arguments) {
  & $Executable @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$Executable failed ($LASTEXITCODE)" }
}

function Read-DxilDump($Log) {
  $lines = @(Get-Content -LiteralPath $Log -Encoding UTF8)
  for ($i = 0; $i -lt $lines.Count; ++$i) {
    if ($lines[$i] -like '*** IR Dump After HLSL DXIL Metadata Emit (hlsl-dxilemit) ***; ModuleID =*') {
      return ($lines[$i..($lines.Count - 1)] -join "`n").TrimEnd()
    }
  }
  throw "No optimized DXIL dump in $Log"
}

function Compare-Frontend($Prefix, $Stage, $Source, $Arguments) {
  $referenceLog = "$Prefix-$Stage-dxc.ll"
  $candidateLog = "$Prefix-$Stage-dxcb5.ll"
  $referenceArgs = $Arguments + @('-T', "${Stage}_6_0", '-Vd', '-validator-version', '0.0',
      '-print-after', 'hlsl-dxilemit', '-Fo', "$Prefix-$Stage.dxil", $Source)
  Invoke-Checked $DXC $referenceArgs *> $referenceLog

  $candidateArgs = $Arguments + @('-T', "${Stage}_5_0", '-print-after', 'hlsl-dxilemit',
      '-Fo', "$Prefix-$Stage-dxcb5.dxbc", $Source)
  if ($Stage -eq 'ps') { $candidateArgs += @('--vs-dxbc', "$Prefix-vs-dxcb5.dxbc") }
  Invoke-Checked $DXCB5 $candidateArgs *> $candidateLog

  if ((Read-DxilDump $referenceLog) -cne (Read-DxilDump $candidateLog)) {
    throw "Optimized DXIL differs. See $referenceLog and $candidateLog"
  }
  Write-Output "$Prefix $Stage optimized DXIL matches"
}

function Test-Variant($Case, $Source, $View, $Level, $Defines) {
  $prefix = Join-Path $OutputDirectory $Case
  foreach ($stage in 'vs', 'ps') {
    $entry = if ($stage -eq 'vs') { 'MT2Vert' } else { 'MT2Frag' }
    $stageFlag = if ($stage -eq 'vs') { 'SHADER_STAGE_VERTEX=1' } else { 'SHADER_STAGE_FRAGMENT=1' }
    $flags = $common + $Defines + $stageFlag
    $dxcb5Args = @('-E', $entry, '-HV', '2016', "-O$Level", '-I', $UnityIncludes)
    $dxcb5Args += @($flags | ForEach-Object { '-D' + $_ })
    $dxcb5Args += '-DUNITY_COMPILER_DXC=1'
    if ($DXC) {
      Compare-Frontend $prefix $stage $Source $dxcb5Args
      continue
    }

    $profile = $stage + '_5_0'
    $fxcArgs = @('/nologo', '/Gec', "/O$Level", '/T', $profile, '/E', $entry, '/I', $UnityIncludes)
    $fxcArgs += @($flags | ForEach-Object { '/D' + $_ })
    Invoke-Checked $FXC ($fxcArgs + @('/Fo', "$prefix-$stage-fxc.dxbc", $Source))

    $dxcb5Args += @('-T', $profile, '-Fo', "$prefix-$stage-dxcb5.dxbc", $Source)
    if ($stage -eq 'ps') { $dxcb5Args += @('--vs-dxbc', "$prefix-vs-dxcb5.dxbc") }
    Invoke-Checked $DXCB5 $dxcb5Args
  }
  if ($DXC) { return }
  Write-Output "Drawing $Case"
  Invoke-Checked $WarpTest @($View, "$prefix-vs-fxc.dxbc", "$prefix-ps-fxc.dxbc",
                              "$prefix-vs-dxcb5.dxbc", "$prefix-ps-dxcb5.dxbc")
}

Test-Variant 'plain' $PlainHlsl 'mono' 1 $plain
Test-Variant 'body-mono' $BodyHlsl 'mono' 3 $body
Test-Variant 'body-stereo' $BodyHlsl 'stereo' 3 ($body + $stereo)