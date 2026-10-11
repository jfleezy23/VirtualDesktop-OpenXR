param(
    [Parameter(Mandatory)][string]$RuntimeObjectDirectory,
    [Parameter(Mandatory)][string]$PrivateRuntimeObjectDirectory,
    [Parameter(Mandatory)][string]$PrivateBackendDirectory,
    [string]$OutputDirectory = 'bin/nr-hardening'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$objects = (Resolve-Path -LiteralPath $RuntimeObjectDirectory).Path
$privateObjects = (Resolve-Path -LiteralPath $PrivateRuntimeObjectDirectory).Path
$backend = (Resolve-Path -LiteralPath $PrivateBackendDirectory).Path
foreach ($name in @('LibOVRRT64_1.dll', 'nvngx_dlssnr.dll', 'virtualdesktop-openxr.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $backend $name))) { throw "Missing private backend input: $name" }
}
if (![IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory = Join-Path $repo $OutputDirectory }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
$results = @()
function Invoke-Case([string]$relative, [string[]]$caseArguments, [string]$name) {
    $stdout = Join-Path $output "$name.stdout.txt"
    $stderr = Join-Path $output "$name.stderr.txt"
    $options = @{FilePath=(Join-Path $repo $relative); PassThru=$true; WindowStyle='Hidden';
                 RedirectStandardOutput=$stdout; RedirectStandardError=$stderr}
    if ($caseArguments.Count) { $options.ArgumentList=@($caseArguments | ForEach-Object { '"' + $_ + '"' }) }
    $process = Start-Process @options
    $null = $process.Handle
    if (!$process.WaitForExit(60000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "Case timed out: $name"
    }
    $process.WaitForExit()
    $text = (Get-Content -LiteralPath $stdout -Raw) + (Get-Content -LiteralPath $stderr -Raw)
    if ($process.ExitCode -ne 0 -or $text -notmatch '(?m)^PASS:' -or
        $text -match '(?m)^(FAIL|ERROR):|ASSERTION FAILED:') {
        throw "Case failed: $name exit=$($process.ExitCode); see $stdout and $stderr"
    }
    $script:results += @{name=$name;exit=$process.ExitCode;stdout=$stdout;stderr=$stderr}
    Write-Output "PASS: $name"
}
& tests/build-nr-command-regression.cmd
if ($LASTEXITCODE) { throw 'Command-context fixture build failed' }
foreach ($mode in @('signal','event','wait','removed','postwait','complete')) {
    Invoke-Case 'bin/tests/nr_command_context_regression.exe' @($mode) "command-$mode"
}
& tests/run-fence-regression.cmd --build-only
if ($LASTEXITCODE) { throw 'Independent queue-fence fixture build failed' }
Invoke-Case 'bin/tests/nr_fence_regression.exe' @() 'independent-fences'
& tests/vr_input_build.cmd -RuntimeObjectDirectory $objects -TestSource nr_retirement_regression.cpp
if ($LASTEXITCODE) { throw 'Retirement fixture build failed' }
foreach ($mode in @('blocked','event')) {
    Invoke-Case 'bin/vr-input-tests/nr_retirement_regression.exe' @($mode) "retirement-$mode"
}
& tests/vr_input_build.cmd -RuntimeObjectDirectory $privateObjects -TestSource nr_input_toggle_regression.cpp
if ($LASTEXITCODE) { throw 'Private real-NR fixture build failed' }
foreach ($coverage in @('90','100')) {
    foreach ($mode in @('toggle','metadata','writes','controls')) {
        $name = "$mode-$coverage"
        Invoke-Case 'bin/vr-input-tests/nr_input_toggle_regression.exe' @($backend,(Join-Path $output "state-$name"),$mode,$coverage) $name
    }
}
@{cases=$results.Count;results=$results;coverage=@(90,100);headset=$false;
  vendor_sha256=(Get-FileHash -LiteralPath (Join-Path $backend 'nvngx_dlssnr.dll')).Hash;
  companion_runtime_sha256=(Get-FileHash -LiteralPath (Join-Path $backend 'virtualdesktop-openxr.dll')).Hash;
  linked_fixture_sha256=(Get-FileHash -LiteralPath (Join-Path $repo 'bin/vr-input-tests/nr_input_toggle_regression.exe')).Hash;
  runtime_objects=$objects;private_runtime_objects=$privateObjects} |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $output 'RESULTS.json') -Encoding UTF8
Write-Output "PASS: $($results.Count) NR hardening process cases (including 32 independent queue-fence waits)"
