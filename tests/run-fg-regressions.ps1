param(
    [ValidateSet('gpu', 'integration', 'presenter', 'lifecycle', 'all')][string]$Suite = 'all',
    [Parameter(Mandatory = $true)][string]$ModuleDirectory,
    [string]$RuntimeCohort,
    [string]$BackendTemplate,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$arguments = @((Join-Path $PSScriptRoot 'run-fg-regressions.py'), '--suite', $Suite,
    '--module-directory', $ModuleDirectory, '--output-directory', $OutputDirectory)
if ($RuntimeCohort) { $arguments += @('--runtime-cohort', $RuntimeCohort) }
if ($BackendTemplate) { $arguments += @('--backend-template', $BackendTemplate) }
& python @arguments
exit $LASTEXITCODE
