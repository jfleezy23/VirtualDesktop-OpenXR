param(
    [ValidateSet('Release', 'ReleaseBundle')][string]$Configuration = 'ReleaseBundle',
    [ValidateSet('Win32', 'x64')][string]$Platform = 'x64',
    [string]$SourceDirectory = '',
    [string]$OutputDirectory = '',
    [string]$MsBuildPath = '',
    [switch]$Analyze
)
$ErrorActionPreference = 'Stop'
if (!$SourceDirectory) { $SourceDirectory = Split-Path -Parent $PSScriptRoot }
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$project = Join-Path $source 'virtualdesktop-openxr/virtualdesktop-openxr.vcxproj'
if (!(Test-Path -LiteralPath $project)) { throw 'The source directory must contain the runtime project.' }
if (!$OutputDirectory) {
    $cohort = if ($Analyze) { 'analyze' } else { 'build' }
    $OutputDirectory = Join-Path $source "bin/community/$cohort/$Platform/$Configuration"
} elseif (![IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $source $OutputDirectory
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
$objects = Join-Path $output 'obj'
if (Test-Path -LiteralPath (Join-Path $objects 'commit.h')) {
    throw 'Use a fresh output directory; mixed build cohorts are not valid regression evidence.'
}
New-Item -ItemType Directory -Force -Path $objects | Out-Null

if (!$MsBuildPath) {
    $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($command) { $MsBuildPath = $command.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (!(Test-Path -LiteralPath $vswhere)) { throw 'MSBuild was not found; install the v143 C++ build tools.' }
        $found = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe')
        if ($LASTEXITCODE -ne 0 -or !$found) { throw 'vswhere could not locate MSBuild with C++ tools.' }
        $MsBuildPath = $found[0]
    }
}
if (!(Test-Path -LiteralPath $MsBuildPath)) { throw 'The selected MSBuild executable does not exist.' }

$commit = & git -C $source rev-parse --verify HEAD
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[a-f0-9]{40}$') { throw 'Cannot determine the source revision.' }
[IO.File]::WriteAllText((Join-Path $objects 'commit.h'), "const char* RuntimeCommitHash = `"$commit`";`n", [Text.Encoding]::ASCII)
$analyzerOption = if ($Analyze) { '/analyze ' } else { '' }
$properties = @"
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup>
    <ClCompile>
      <FloatingPointModel>Precise</FloatingPointModel>
      <AdditionalOptions>$analyzerOption%(AdditionalOptions)</AdditionalOptions>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
"@
$propertiesPath = Join-Path $output 'community-build.props'
[IO.File]::WriteAllText($propertiesPath, $properties, [Text.Encoding]::UTF8)
# A terminal backslash escapes the closing native argument quote in Windows PowerShell 5.1.
# MSBuild accepts forward slashes; retain the native physical paths for filesystem operations.
$sourceProperty = $source.Replace('\', '/') + '/'
$outputProperty = $output.Replace('\', '/') + '/'
$objectsProperty = $objects.Replace('\', '/') + '/'
$arguments = @(
    $project, '/t:Rebuild', '/m', '/nologo', '/v:minimal',
    "/p:Configuration=$Configuration", "/p:Platform=$Platform", "/p:SolutionDir=$sourceProperty",
    "/p:OutDir=$outputProperty", "/p:IntDir=$objectsProperty", "/p:ForceImportBeforeCppTargets=$propertiesPath",
    '/p:PreBuildEventUseInBuild=false', '/p:PostBuildEventUseInBuild=false',
    '/fl', "/flp:LogFile=$output/build.log;Verbosity=normal;NoSummary"
)
if ($Analyze) { $arguments += '/p:WholeProgramOptimization=false' }
& $MsBuildPath @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "PASS: core $Configuration|$Platform (analyze=$Analyze); objects: $objects"
