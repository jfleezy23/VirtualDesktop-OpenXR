param(
    [ValidateSet('current','baseline','patched')][string]$Variant='current',
    [string]$SourceDirectory='',
    [string]$RuntimeObjectDirectory=''
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo "bin/tests/gamma-$Variant"
New-Item -ItemType Directory -Force -Path $output | Out-Null
if($Variant-eq 'current'){
    # Runtime sources/headers and objects MUST come from one completed build.
    # Historical variants below replay the separately retained audit cohorts.
    if(!$SourceDirectory){$SourceDirectory=Join-Path $repo 'virtualdesktop-openxr'}
    if(!$RuntimeObjectDirectory){$RuntimeObjectDirectory=Join-Path $repo 'bin/nr-null-patched/obj'}
    & (Join-Path $PSScriptRoot 'vr_input_build.cmd') -SourceDirectory $SourceDirectory -RuntimeObjectDirectory $RuntimeObjectDirectory -TestSource nr_gamma_mapping_regression.cpp
    if($LASTEXITCODE){exit $LASTEXITCODE}
    Copy-Item -LiteralPath (Join-Path $repo 'bin/vr-input-tests/nr_gamma_mapping_regression.exe') -Destination (Join-Path $output 'nr_gamma_mapping_regression.exe')
    Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $SourceDirectory 'runtime.h'),(Join-Path $output 'nr_gamma_mapping_regression.exe')
    exit 0
}
$source=if($Variant-eq 'baseline'){Join-Path $output 'source'}else{Join-Path $repo 'bin/tests/gamma-baseline/source'}
$objects=Join-Path $output 'objects'
if($Variant-eq 'baseline'){
    if (Test-Path -LiteralPath $source) { throw "Refusing to replace an existing source snapshot: $source" }
    Copy-Item -LiteralPath (Join-Path $repo 'virtualdesktop-openxr') -Destination $source -Recurse
}
New-Item -ItemType Directory -Force -Path $objects | Out-Null
$objectSource=if($Variant-eq 'baseline'){Join-Path $repo 'bin/nr-null-patched/obj'}else{Join-Path $repo 'bin/tests/gamma-baseline/objects'}
Get-ChildItem -LiteralPath $objectSource -File | Where-Object {$_.Extension-in '.obj','.h'} | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $objects
}
if($Variant-eq 'patched'){
    if(!(Test-Path -LiteralPath (Join-Path $output 'overrides'))){throw 'Historical patched replay requires retained gamma-baseline/source and gamma-patched/overrides from the audit; use current for a fresh runtime build.'}
    & (Join-Path $PSScriptRoot 'nr_gamma_compile_overrides.cmd') -SourceDirectory $source -RuntimeObjectDirectory $objects -OverrideDirectory (Join-Path $output 'overrides')
    if($LASTEXITCODE){exit $LASTEXITCODE}
}
& (Join-Path $PSScriptRoot 'vr_input_build.cmd') -SourceDirectory $source -RuntimeObjectDirectory $objects -TestSource nr_gamma_mapping_regression.cpp
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Copy-Item -LiteralPath (Join-Path $repo 'bin/vr-input-tests/nr_gamma_mapping_regression.exe') -Destination (Join-Path $output 'nr_gamma_mapping_regression.exe')
Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $source 'runtime.h'),(Join-Path $output 'nr_gamma_mapping_regression.exe')
