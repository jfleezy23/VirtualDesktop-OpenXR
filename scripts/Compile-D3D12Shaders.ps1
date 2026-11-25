#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$DxcPath=$env:VDXR_DXC_PATH,
    [Parameter(Mandatory)][string]$SourceDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
# A PS5 child of PS7 can inherit PS7's module search path. Load this host's
# inbox hashing/JSON commands explicitly without changing the environment.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
$source=(Resolve-Path -LiteralPath $SourceDirectory).Path
$output=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
[void](New-Item -ItemType Directory -Path $output -Force)
$manifest=Join-Path $output 'D3D12-SHADERS.json'
if(Test-Path -LiteralPath $manifest){Remove-Item -LiteralPath $manifest -Force}
if(-not $DxcPath){throw 'Select the pinned DXC with -DxcPath or VDXR_DXC_PATH; no compiler fallback exists.'}
$dxc=(Resolve-Path -LiteralPath $DxcPath).Path
$toolDirectory=Split-Path -Parent $dxc
$pins=@{
    'dxc.exe'='aa7965b3763d9c710baaa5a28cedcc633449962a75d1391d161a85304a41b3e2'
    'dxcompiler.dll'='0689e2fee0a1c9df8662a59680469f24e2a3ad7ce6bbbcc9284f7a83172bfc59'
    'dxil.dll'='64be2368211d257a038a7d4ea6bb3e24c878eb8d588c7b8414fccf759b69c2ac'
}
$version=(Get-Item -LiteralPath $dxc).VersionInfo
if($version.FileVersion -cne '1.9.2609.5'){throw 'Selected executable is not the pinned DXC 1.9.2609.5.'}
$tools=@{}
foreach($name in @('dxc.exe','dxcompiler.dll','dxil.dll')){
    $path=if($name -ceq 'dxc.exe'){$dxc}else{Join-Path $toolDirectory $name}
    $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if($hash -cne $pins[$name]){throw ('Pinned shader tool hash differs: '+$name)}
    $tools[$name]=@{name=$name;path=$path;sha256=$hash;version=(Get-Item -LiteralPath $path).VersionInfo.FileVersion}
}
$recipes=[ordered]@{
    NrSpatialDownsampleCS=@('NrSpatialDownsampleCS','cs_6_0','main')
    NrSpatialResolvePS=@('NrSpatialResolvePS','ps_6_0','main')
    NrNativeResolvePS=@('NrNativeResolvePS','ps_6_0','main')
    FullScreenQuadVS=@('FullScreenQuadVS','vs_6_0','main')
    NrBeforeSrColorEncode=@('NrBeforeSrColorCS','cs_6_0','Encode')
    NrBeforeSrColorResolve=@('NrBeforeSrColorCS','cs_6_0','Resolve')
}
$inputHashes=@{}
function Read-ShaderInput([string]$Path){
    $path=(Resolve-Path -LiteralPath $Path).Path
    if($inputHashes.ContainsKey($path)){return}
    $inputHashes[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $text=[IO.File]::ReadAllText($path)
    foreach($include in [regex]::Matches($text,'(?m)^\s*#\s*include\s*"([^"]+)"')){
        Read-ShaderInput (Join-Path (Split-Path -Parent $path) $include.Groups[1].Value)
    }
}
$shaderSources=@($recipes.Values|ForEach-Object {$_[0]+'.hlsl'})
foreach($name in $shaderSources){Read-ShaderInput (Join-Path $source $name)}
$stage=Join-Path $output ('.dxc-'+[guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $stage)
$temporary=[Collections.Generic.List[string]]::new()
$shaders=[Collections.Generic.List[object]]::new()
try{
    foreach($name in $recipes.Keys){
        $header=$name+'_DXIL.h';$symbol='g_'+$name+'_DXIL'
        $headerPath=Join-Path $stage $header;$objectPath=Join-Path $stage ($name+'.dxil')
        $temporary.Add($headerPath);$temporary.Add($objectPath)
        $shaderSource,$profile,$entry=$recipes[$name]
        $arguments=@('-T',$profile,'-E',$entry,'-HV','2016','-O3','-Ges','-WX',
            '-Qstrip_debug','-Qstrip_reflect','-Fo',$objectPath,'-Fh',$headerPath,'-Vn',$symbol,
            (Join-Path $source ($shaderSource+'.hlsl')))
        if($shaderSource -ceq 'NrBeforeSrColorCS'){$arguments=@('-Gis')+$arguments}
        $previousErrorAction=$ErrorActionPreference
        try{
            $ErrorActionPreference='Continue'
            $diagnostics=(& $dxc @arguments 2>&1|Out-String)
            $exitCode=$LASTEXITCODE
        }finally{$ErrorActionPreference=$previousErrorAction}
        if($exitCode -ne 0){throw ('DXC compilation failed for '+$name+' (exit '+$exitCode+'): '+$diagnostics)}
        if(-not (Test-Path -LiteralPath $headerPath -PathType Leaf) -or
           -not (Test-Path -LiteralPath $objectPath -PathType Leaf) -or (Get-Item -LiteralPath $headerPath).Length -eq 0){
            throw ('DXC did not produce both outputs for '+$name)
        }
        $shaders.Add(@{source=$shaderSource+'.hlsl';profile=$profile;entry=$entry;header=$header;symbol=$symbol;
            arguments=$arguments;exit_code=$exitCode;diagnostics=$diagnostics;
            header_sha256=(Get-FileHash -LiteralPath $headerPath).Hash.ToLowerInvariant();
            dxil_sha256=(Get-FileHash -LiteralPath $objectPath).Hash.ToLowerInvariant()})
    }
    foreach($path in $inputHashes.Keys){
        if((Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -cne $inputHashes[$path]){throw 'Shader input changed during compilation.'}
    }
    foreach($tool in $tools.Values){
        if((Get-FileHash -LiteralPath $tool.path).Hash.ToLowerInvariant() -cne $tool.sha256){throw 'Shader compiler changed during compilation.'}
    }
    $inputs=@();$includes=@()
    $sourceUri=[uri]($source.TrimEnd([char[]]'\/')+[IO.Path]::DirectorySeparatorChar)
    foreach($path in @($inputHashes.Keys|Sort-Object)){
        $relative=[uri]::UnescapeDataString($sourceUri.MakeRelativeUri([uri]$path).ToString()).Replace('/','\')
        $record=@{file=$relative;sha256=$inputHashes[$path]}
        if([IO.Path]::GetFileName($path) -in $shaderSources){
            $inputs+=$record
        }else{$includes+=$record}
    }
    $receipt=@{schema=1;success=$true;format='DXIL';hlsl_version='2016';source_directory=$source;
        compiler=@{path=$dxc;version=$version.FileVersion;product_version=$version.ProductVersion;
            sha256=$tools['dxc.exe'].sha256;helpers=@($tools['dxcompiler.dll'],$tools['dxil.dll'])};
        inputs=$inputs;includes=$includes;shaders=$shaders.ToArray();
        builder_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash.ToLowerInvariant();utc=[DateTime]::UtcNow}
    $pendingManifest=Join-Path $stage 'D3D12-SHADERS.json';$temporary.Add($pendingManifest)
    $receipt|ConvertTo-Json -Depth 9|Set-Content -LiteralPath $pendingManifest -Encoding utf8
    foreach($shader in $shaders){
        $destination=Join-Path $output $shader.header
        if(Test-Path -LiteralPath $destination){
            if((Get-Item -LiteralPath $destination).PSIsContainer){throw 'Shader header destination is a directory.'}
            if((Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant() -ceq $shader.header_sha256){continue}
        }
        Move-Item -LiteralPath (Join-Path $stage $shader.header) -Destination $destination -Force
    }
    Move-Item -LiteralPath $pendingManifest -Destination $manifest -Force
    Write-Output ("Built $($shaders.Count) DXIL shader headers with pinned DXC "+$version.FileVersion)
}finally{
    foreach($path in $temporary){if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force}}
    if(Test-Path -LiteralPath $stage){Remove-Item -LiteralPath $stage}
}
