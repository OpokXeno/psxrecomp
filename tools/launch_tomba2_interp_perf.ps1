param(
    [ValidateSet('full', 'hybrid')]
    [string]$Mode = 'hybrid',
    [switch]$Launch,
    [int]$DebugPort = 4615,
    [string]$GameRoot = ''
)

$ErrorActionPreference = 'Stop'

if ($DebugPort -lt 1 -or $DebugPort -gt 65535) {
    throw "DebugPort must be in the range 1..65535 (received $DebugPort)."
}

function Quote-ProcessArgument([string]$Value) {
    return '"' + $Value.Replace('"', '\"') + '"'
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$workspaceRoot = Split-Path $repoRoot -Parent
if (-not $GameRoot) {
    $GameRoot = Join-Path $workspaceRoot 'Tomba2Recomp'
}
$GameRoot = (Resolve-Path $GameRoot).Path

$buildDir = Join-Path $repoRoot 'runtime\build-tomba2-perf-rel'
$sourceExe = Join-Path $buildDir 'Tomba2Recomp.exe'
$isolatedExe = Join-Path $buildDir 'Tomba2InterpPerf.exe'
$gameConfig = Join-Path $GameRoot 'game.toml'
$bios = Join-Path $workspaceRoot 'psxrecomp\bios\SCPH1001.BIN'
$recompiler = Join-Path $repoRoot 'recompiler\build-interpreter-perf-native\psxrecomp-game.exe'
$compileTool = Join-Path $repoRoot 'tools\compile_overlays.py'
$runtimeInclude = Join-Path $repoRoot 'runtime\include'

foreach ($required in @($sourceExe, $gameConfig, $bios, $recompiler, $compileTool)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required launch input is missing: $required"
    }
}

if ($Launch -and (Get-Process -Name 'Tomba2InterpPerf' -ErrorAction SilentlyContinue)) {
    throw 'Tomba2InterpPerf is already running; refusing to replace its executable or start another instance.'
}

# Keep the ordinary Tomba2Recomp.exe for build tooling, but launch a uniquely
# named byte-identical alias so process-name based diagnostics/cleanup cannot
# touch an AOT investigation running from another build directory.
Copy-Item -LiteralPath $sourceExe -Destination $isolatedExe -Force

$sessionRoot = Join-Path $repoRoot '.local\tomba2-interp-perf'
$stateDir = Join-Path $sessionRoot 'state'
$capturePath = Join-Path $sessionRoot 'captures\SCUS-94454\overlay_captures.json'
$runDir = Join-Path $sessionRoot (Join-Path 'run' $Mode)
$logDir = Join-Path $sessionRoot 'logs'
$cacheDir = Join-Path $buildDir 'cache'
foreach ($dir in @($stateDir, (Split-Path $capturePath -Parent), $runDir, $logDir, $cacheDir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

$autoCompile = @(
    'python', (Quote-ProcessArgument $compileTool),
    '--captures', (Quote-ProcessArgument $capturePath),
    '--game-toml', (Quote-ProcessArgument $gameConfig),
    '--recompiler', (Quote-ProcessArgument $recompiler),
    '--runtime-include', (Quote-ProcessArgument $runtimeInclude),
    '--out-dir', (Quote-ProcessArgument $cacheDir)
) -join ' '

$childEnvironment = @{
    PSX_NO_LAUNCHER = '1'
    PSX_OVERLAY_CAPTURES = $capturePath
    PSX_OVERLAY_AUTOCOMPILE_CWD = $GameRoot
    PSX_FRAME_INTERPOLATION = '0'
}
if ($Mode -eq 'full') {
    $childEnvironment.PSX_FORCE_INTERP = '1'
    $childEnvironment.PSX_OVERLAY_NATIVE_OFF = '1'
    # Keep the full-interpreter measurement free of background compilation
    # load. Capture remains additive for later hybrid regeneration.
    $childEnvironment.PSX_OVERLAY_AUTOCOMPILE_OFF = '1'
} else {
    $childEnvironment.PSX_FORCE_INTERP = '0'
    $childEnvironment.PSX_OVERLAY_NATIVE_OFF = '0'
    $childEnvironment.PSX_OVERLAY_AUTOCOMPILE_OFF = '0'
    $childEnvironment.PSX_OVERLAY_BACKEND = 'gcc'
    $childEnvironment.PSX_OVERLAY_AUTOCOMPILE_CMD = $autoCompile
}

$arguments = @(
    '--bios', $bios,
    '--game', $gameConfig,
    '--debug-port', $DebugPort.ToString(),
    '--window-title', "Tomba 2 - $Mode interpreter perf",
    '--memcard-dir', $stateDir,
    '--no-launcher'
)
$argumentLine = ($arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join ' '

Write-Output "Prepared isolated executable: $isolatedExe"
Write-Output "Mode: $Mode; debug port: $DebugPort; writable state: $stateDir"
Write-Output "Overlay cache: $cacheDir; additive captures: $capturePath"

if (-not $Launch) {
    Write-Output 'Preparation only; no process was launched. Pass -Launch explicitly to start it.'
    exit 0
}

if (Get-Command Get-NetTCPConnection -ErrorAction SilentlyContinue) {
    if (Get-NetTCPConnection -State Listen -LocalPort $DebugPort -ErrorAction SilentlyContinue) {
        throw "Debug port $DebugPort is already listening; choose another -DebugPort."
    }
}

$savedEnvironment = @{}
try {
    foreach ($name in $childEnvironment.Keys) {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $childEnvironment[$name], 'Process')
    }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $stdout = Join-Path $logDir "$Mode-$stamp.stdout.log"
    $stderr = Join-Path $logDir "$Mode-$stamp.stderr.log"
    $process = Start-Process -FilePath $isolatedExe -ArgumentList $argumentLine `
        -WorkingDirectory $runDir -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -WindowStyle Normal -PassThru
    Write-Output "Launched PID $($process.Id); stdout=$stdout; stderr=$stderr"
} finally {
    foreach ($name in $childEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
