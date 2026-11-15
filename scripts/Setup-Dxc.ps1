#Requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
$output=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if(Test-Path -LiteralPath $output){throw 'DXC setup requires a fresh output directory.'}
[void](New-Item -ItemType Directory -Path $output)
$url='https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip'
$archive=Join-Path $output 'dxc_2026_09_29.zip'
$archiveHash='ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1'
Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
if((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant() -cne $archiveHash){
    throw 'The pinned DXC archive checksum differs.'
}
Expand-Archive -LiteralPath $archive -DestinationPath $output
$pins=@{
    'dxc.exe'='aa7965b3763d9c710baaa5a28cedcc633449962a75d1391d161a85304a41b3e2'
    'dxcompiler.dll'='0689e2fee0a1c9df8662a59680469f24e2a3ad7ce6bbbcc9284f7a83172bfc59'
    'dxil.dll'='64be2368211d257a038a7d4ea6bb3e24c878eb8d588c7b8414fccf759b69c2ac'
}
$tools=@()
foreach($name in @('dxc.exe','dxcompiler.dll','dxil.dll')){
    $path=Join-Path $output "bin\x64\$name"
    $hash=(Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    if($hash -cne $pins[$name]){throw "Extracted DXC tool checksum differs: $name"}
    $tools+=@{file=$name;path=$path;sha256=$hash;file_version=(Get-Item -LiteralPath $path).VersionInfo.FileVersion}
}
$dxc=Join-Path $output 'bin\x64\dxc.exe'
if((Get-Item -LiteralPath $dxc).VersionInfo.FileVersion -cne '1.9.2609.5'){
    throw 'The extracted compiler has an unexpected version.'
}
$receipt=@{schema=1;success=$true;url=$url;archive=$archive;archive_sha256=$archiveHash;tools=$tools;utc=[DateTime]::UtcNow.ToString('o')}
$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'DXC-SETUP.json') -Encoding UTF8
Write-Output $dxc
