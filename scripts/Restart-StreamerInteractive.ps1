param(
    [Parameter(Mandatory = $true)][ValidateSet('Check', 'Stop', 'Restart', 'Launch', 'Quiesced')][string]$Action,
    [Parameter(Mandatory = $true)][string]$ConfigPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)

# This helper performs no registry or launch-cache writes. The Python caller
# keeps the switch mutex and rolls back its transaction on a failed handoff.
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class VdxrLaunchToken {
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, int id);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token, int info, out int value, int size, out int returned);
    public static bool IsElevated(int processId) {
        IntPtr process = OpenProcess(0x1000, false, processId);
        if (process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        IntPtr token = IntPtr.Zero;
        try {
            if (!OpenProcessToken(process, 8, out token)) throw new Win32Exception(Marshal.GetLastWin32Error());
            int value, returned;
            if (!GetTokenInformation(token, 20, out value, sizeof(int), out returned)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return value != 0;
        } finally {
            if (token != IntPtr.Zero) CloseHandle(token);
            CloseHandle(process);
        }
    }
}
'@

function Get-Configuration {
    $expected = Join-Path $env:ProgramFiles 'VDXR-NR-FG/LaunchTrees/config.json'
    if ([IO.Path]::GetFullPath($ConfigPath) -ine [IO.Path]::GetFullPath($expected)) {
        throw 'Expected the administrator-installed launch-tree configuration.'
    }
    $config = Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($config.schema -ne 1) { throw 'Unsupported launch-tree configuration.' }
    $config.streamer_path = [IO.Path]::GetFullPath($config.streamer_path)
    $config
}

function Get-Identity($config) {
    $sid = ($config.library.registry_path -split '\\')[0]
    if ($sid -notmatch '^S-1-(?:5-21|12-1)-[0-9-]+$') { throw 'Expected an explicit interactive user SID.' }
    if ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -ne $sid) {
        throw 'The launch-tree helper must run as the configured interactive user; over-the-shoulder elevation is unsupported.'
    }
    $session = (Get-Process -Id $PID).SessionId
    $explorers = @(Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" |
        Where-Object { $_.SessionId -eq $session })
    foreach ($explorer in $explorers) {
        $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwnerSid
        if ($owner.ReturnValue -eq 0 -and $owner.Sid -eq $sid) {
            return @{ sid = $sid; session = $session }
        }
    }
    throw 'The configured user does not have an interactive desktop in this session.'
}

function Get-VDProcesses($config) {
    $directory = Split-Path -Parent $config.streamer_path
    $owned = @()
    foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='VirtualDesktop.Streamer.exe' OR Name='VirtualDesktop.Server.exe'")) {
        if (-not $process.ExecutablePath) { throw 'Cannot verify an existing Virtual Desktop process path.' }
        if (($process.ExecutablePath -ieq $config.streamer_path) -or
            ($process.ExecutablePath -ieq (Join-Path $directory 'VirtualDesktop.Server.exe'))) {
            $owned += $process
        }
    }
    $owned
}

function Get-Status($config, $identity, [bool]$inspectGames) {
    $streamers = @()
    $servers = @()
    foreach ($process in @(Get-VDProcesses $config)) {
        if ($process.Name -ieq 'VirtualDesktop.Streamer.exe') {
            $owner = Invoke-CimMethod -InputObject $process -MethodName GetOwnerSid
            if ($owner.ReturnValue -ne 0 -or $owner.Sid -ne $identity.sid -or $process.SessionId -ne $identity.session) {
                throw 'Virtual Desktop Streamer belongs to a different interactive session.'
            }
            $streamers += [int]$process.ProcessId
        } else { $servers += [int]$process.ProcessId }
    }
    $games = @()
    if ($inspectGames) {
        $paths = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($path in @($config.game_executables)) {
            [void]$paths.Add([IO.Path]::GetFullPath($path))
            [void]$names.Add([IO.Path]::GetFileName($path))
        }
        foreach ($process in @(Get-CimInstance Win32_Process)) {
            if ($process.ProcessId -eq $PID -or $streamers -contains $process.ProcessId -or $servers -contains $process.ProcessId) { continue }
            if (-not $process.ExecutablePath -and $names.Contains($process.Name)) {
                throw 'Close the possible VR game whose process path cannot be inspected.'
            }
            $match = $process.ExecutablePath -and $paths.Contains($process.ExecutablePath)
            if (-not $match) {
                try {
                    $match = @((Get-Process -Id $process.ProcessId -ErrorAction Stop).Modules |
                        Where-Object { $_.ModuleName -ieq 'virtualdesktop-openxr.dll' }).Count -gt 0
                } catch {
                    # Protected unrelated system processes are not VR games.
                    # Known executable paths above remain independently checked.
                }
            }
            if ($match) { $games += [int]$process.ProcessId }
        }
    }
    @{ streamer_pids = @($streamers); server_pids = @($servers); games = @($games) }
}

function Stop-Streamer($config, $identity) {
    $status = Get-Status $config $identity $true
    if ($status.games.Count -gt 0) { throw 'Close the running VR game before restarting Streamer.' }
    if ($status.streamer_pids.Count -eq 0 -and $status.server_pids.Count -eq 0) { return }
    $exitProcess = Start-Process -FilePath $config.streamer_path -ArgumentList '-exit' -WindowStyle Hidden -PassThru
    try {
        if (-not $exitProcess.WaitForExit(5000)) { throw 'Streamer did not acknowledge its normal exit command.' }
    } finally { $exitProcess.Dispose() }
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if (@(Get-VDProcesses $config).Count -eq 0) { return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Streamer/server did not exit; launch trees were not changed.'
}

function Assert-NormalStreamer($config, $identity, [int]$processId) {
    $process = Get-CimInstance Win32_Process -Filter "ProcessId=$processId"
    if (-not $process -or $process.ExecutablePath -ine $config.streamer_path -or $process.SessionId -ne $identity.session) {
        throw 'Restart did not produce Streamer in the configured interactive session.'
    }
    $owner = Invoke-CimMethod -InputObject $process -MethodName GetOwnerSid
    if ($owner.ReturnValue -ne 0 -or $owner.Sid -ne $identity.sid -or [VdxrLaunchToken]::IsElevated($processId)) {
        throw 'Restart did not produce Streamer under the normal user token.'
    }
}

function Restart-Streamer($config, $identity) {
    $existing = Get-Status $config $identity $false
    if ($existing.streamer_pids.Count -gt 0) {
        foreach ($processId in $existing.streamer_pids) { Assert-NormalStreamer $config $identity $processId }
        return @{ restarted = $false; normal_user = $true; streamer_pids = $existing.streamer_pids }
    }
    $name = 'VDXR-LaunchTrees-' + [Guid]::NewGuid().ToString('N')
    $powershell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "' +
        $PSCommandPath + '" -Action Launch -ConfigPath "' + $ConfigPath + '"'
    $taskAction = New-ScheduledTaskAction -Execute $powershell -Argument $arguments
    $principal = New-ScheduledTaskPrincipal -UserId $identity.sid -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
    $registered = $false
    try {
        Register-ScheduledTask -TaskName $name -Action $taskAction -Principal $principal -Settings $settings | Out-Null
        $registered = $true
        Start-ScheduledTask -TaskName $name
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        do {
            $status = Get-Status $config $identity $false
            if ($status.streamer_pids.Count -gt 0) {
                foreach ($processId in $status.streamer_pids) { Assert-NormalStreamer $config $identity $processId }
                return @{ restarted = $true; normal_user = $true; streamer_pids = $status.streamer_pids }
            }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        throw 'The one-shot normal-user restart did not start Streamer.'
    } finally {
        if ($registered) { Unregister-ScheduledTask -TaskName $name -Confirm:$false }
    }
}

try {
    $config = Get-Configuration
    $identity = Get-Identity $config
    switch ($Action) {
        'Check' { Get-Status $config $identity $true | ConvertTo-Json -Compress -Depth 4 }
        'Quiesced' { Get-Status $config $identity $false | ConvertTo-Json -Compress -Depth 4 }
        'Stop' { Stop-Streamer $config $identity; @{ stopped = $true } | ConvertTo-Json -Compress }
        'Restart' { Restart-Streamer $config $identity | ConvertTo-Json -Compress -Depth 4 }
        'Launch' {
            if ([VdxrLaunchToken]::IsElevated($PID)) { throw 'The one-shot launch helper must run unelevated.' }
            if ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value -ne $identity.sid) { throw 'Wrong user for Streamer launch.' }
            Start-Process -FilePath $config.streamer_path -ArgumentList '-StartMinimized' -WindowStyle Hidden | Out-Null
            @{ launched = $true } | ConvertTo-Json -Compress
        }
    }
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
