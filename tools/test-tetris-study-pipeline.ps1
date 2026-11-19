param(
    [switch]$OnlineYtDlp
)

$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$runtime = Join-Path $tools 'video-recovery-runtime'
$root = Split-Path -Parent $tools
$failures = New-Object System.Collections.Generic.List[string]

function Test-Requirement {
    param([bool]$Condition, [string]$Message)
    if ($Condition) {
        Write-Host "OK  $Message" -ForegroundColor Green
    } else {
        Write-Host "NG  $Message" -ForegroundColor Red
        $failures.Add($Message)
    }
}

function Find-Tool {
    param([string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) { return $null }
    return $command.Source
}

$study = Join-Path $runtime 'bin\TetrisStudy.exe'
$streamScript = Join-Path $tools 'run-ppt-stream-analysis.ps1'
$fakeBatch = Join-Path $tools 'tests\fake-ppt-batch.cmd'
$smokeVideo = Join-Path $runtime 'smoke\final_runtime_smoke_6.mov'

Test-Requirement (Test-Path -LiteralPath $study -PathType Leaf) 'TetrisStudy.exe exists'
Test-Requirement (Test-Path -LiteralPath $streamScript -PathType Leaf) 'stream analysis wrapper exists'
Test-Requirement (Test-Path -LiteralPath $fakeBatch -PathType Leaf) 'machine protocol fixture exists'
Test-Requirement (Test-Path -LiteralPath $smokeVideo -PathType Leaf) 'small local smoke video exists'

$ytDlp = Find-Tool 'yt-dlp.exe'
$deno = Find-Tool 'deno.exe'
$ffmpeg = Find-Tool 'ffmpeg.exe'
$ffprobe = Find-Tool 'ffprobe.exe'
$tar = Find-Tool 'tar.exe'
Test-Requirement (-not [string]::IsNullOrWhiteSpace($ytDlp)) 'yt-dlp.exe is discoverable'
Test-Requirement (-not [string]::IsNullOrWhiteSpace($deno)) 'Deno JavaScript runtime is discoverable'
Test-Requirement (-not [string]::IsNullOrWhiteSpace($ffmpeg)) 'ffmpeg.exe is discoverable'
Test-Requirement (-not [string]::IsNullOrWhiteSpace($ffprobe)) 'ffprobe.exe is discoverable'
Test-Requirement (-not [string]::IsNullOrWhiteSpace($tar)) 'tar.exe is discoverable for first-use Deno setup'

if ($ytDlp) {
    $help = & $ytDlp --ignore-config --help | Out-String
    Test-Requirement ($help -match '--progress-template') 'yt-dlp supports machine progress'
    Test-Requirement ($help -match '--print-to-file') 'yt-dlp supports after-move path output'
    Test-Requirement ($help -match '--js-runtimes') 'yt-dlp supports an external JS runtime'
}

$audit = Join-Path ([IO.Path]::GetTempPath()) ('tetris-study-pipeline-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $audit | Out-Null
try {
    if ((Test-Path -LiteralPath $smokeVideo) -and (Test-Path -LiteralPath $fakeBatch)) {
        $testOutput = Join-Path $audit 'batch-output'
        $testSceneModel = Join-Path $audit 'scene-model.onnx'
        $testBoardModel = Join-Path $audit 'board-model.onnx'
        $testSettings = Join-Path $audit 'settings.ini'
        [IO.File]::WriteAllText($testSceneModel, 'scene-v1')
        [IO.File]::WriteAllText($testBoardModel, 'board-v1')
        [IO.File]::WriteAllText($testSettings, 'settings-v1')
        $testArguments = @(
            '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass',
            '-File', $streamScript, '-VideoPath', $smokeVideo, '-MachineReadable',
            # 偽のエンジン・偽のモデルで機械プロトコルだけを見る試験なので、
            # 実環境のファイルを要求する解析前チェックは省略する。
            '-SkipEnvironmentCheck',
            '-BatchExecutable', $fakeBatch, '-OutputDirectory', $testOutput,
            '-SceneModelPath', $testSceneModel, '-BoardModelPath', $testBoardModel,
            '-SettingsPath', $testSettings
        )
        $machineOutput = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass `
            -File $streamScript -VideoPath $smokeVideo -MachineReadable `
            -SkipEnvironmentCheck `
            -BatchExecutable $fakeBatch -OutputDirectory $testOutput `
            -SceneModelPath $testSceneModel -BoardModelPath $testBoardModel `
            -SettingsPath $testSettings 2>&1 | Out-String
        Test-Requirement ($LASTEXITCODE -eq 0) 'stream wrapper machine mode exits successfully'
        Test-Requirement ($machineOutput -match '(?m)^study_progress=0\s') 'machine output contains initial progress'
        Test-Requirement ($machineOutput -match '(?m)^study_progress=100\s') 'machine output contains final progress'
        Test-Requirement ($machineOutput -match '(?m)^study_manifest=.+batch-results\.json\s*$') `
            'machine output passes the result manifest path'
        Test-Requirement ($machineOutput -match '(?m)^study_cache=stored\s') `
            'first run stores a versioned full-analysis cache'

        $invocations = Join-Path $testOutput 'fake-batch-invocations.txt'
        Test-Requirement (@(Get-Content -LiteralPath $invocations).Count -eq 1) `
            'first run invokes full analysis once'
        $reuseOutput = & powershell.exe @testArguments 2>&1 | Out-String
        Test-Requirement ($LASTEXITCODE -eq 0) 'exact-version reuse exits successfully'
        Test-Requirement ($reuseOutput -match '(?m)^study_cache=hit\s') `
            'same video and version reuse the full previous result'
        Test-Requirement (@(Get-Content -LiteralPath $invocations).Count -eq 1) `
            'cache hit does not invoke full analysis again'

        [IO.File]::AppendAllText($testBoardModel, '-changed')
        $modelChangeOutput = & powershell.exe @testArguments 2>&1 | Out-String
        Test-Requirement ($LASTEXITCODE -eq 0) 'model-change reanalysis exits successfully'
        Test-Requirement ($modelChangeOutput -match 'study_cache=miss reason=board_model-changed') `
            'board-model change invalidates the previous result'
        Test-Requirement (@(Get-Content -LiteralPath $invocations).Count -eq 2) `
            'model change invokes full analysis again'

        $cacheRecordPath = Join-Path $testOutput 'analysis-cache.v1.json'
        $cacheRecord = Get-Content -LiteralPath $cacheRecordPath -Raw | ConvertFrom-Json
        $cacheRecord.analysis_version = 'obsolete-test-version'
        $cacheRecord | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $cacheRecordPath -Encoding UTF8
        $versionChangeOutput = & powershell.exe @testArguments 2>&1 | Out-String
        Test-Requirement ($LASTEXITCODE -eq 0) 'analysis-version reanalysis exits successfully'
        Test-Requirement ($versionChangeOutput -match 'study_cache=miss reason=analysis-version-changed') `
            'analysis-version change invalidates the previous result'
        Test-Requirement (@(Get-Content -LiteralPath $invocations).Count -eq 3) `
            'analysis-version change invokes full analysis again'
    }

    if ($OnlineYtDlp -and $ytDlp) {
        if ($deno -and $ffmpeg) {
            # Resolve a real YouTube format without downloading media. This is
            # the closest cheap check of the exact Deno + FFmpeg app command.
            $simulateArguments = @(
                '--ignore-config', '--simulate', '--newline', '--no-colors',
                '--no-playlist', '--windows-filenames', '--encoding', 'utf-8',
                '--ffmpeg-location', (Split-Path -Parent $ffmpeg),
                '--merge-output-format', 'mp4',
                '-f', 'bestvideo[height<=1080]+bestaudio/best[height<=1080]',
                '--js-runtimes', ('deno:' + $deno),
                'https://www.youtube.com/watch?v=dQw4w9WgXcQ'
            )
            $previousErrorActionPreference = $ErrorActionPreference
            try {
                $ErrorActionPreference = 'Continue'
                $simulateOutput = & $ytDlp @simulateArguments 2>&1 | Out-String
                $simulateExitCode = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $previousErrorActionPreference
            }
            Test-Requirement ($simulateExitCode -eq 0) 'YouTube app command resolves in simulate mode'
            Test-Requirement ($simulateOutput -match '\[jsc:deno\]') 'YouTube JS challenge uses Deno'
            Test-Requirement ($simulateOutput -match 'Downloading 1 format') '1080p format expression selects a format'
        }

        # This tiny official yt-dlp text asset exercises the real download
        # protocol without downloading a stream video.
        $downloadDirectory = Join-Path $audit 'yt-dlp'
        New-Item -ItemType Directory -Path $downloadDirectory | Out-Null
        $resultFile = Join-Path $downloadDirectory 'after-move.txt'
        $arguments = @(
            '--ignore-config', '--newline', '--no-playlist', '--windows-filenames',
            '--encoding', 'utf-8',
            '--progress-template', 'download:__YTDLP_PROGRESS__:%(progress._percent_str)s|%(progress._speed_str)s|%(progress._eta_str)s',
            '--print-to-file', 'after_move:%(filepath)s', $resultFile,
            '--paths', $downloadDirectory,
            '-o', '%(title).120B [%(id)s].%(ext)s'
        )
        if ($deno) { $arguments += @('--js-runtimes', 'deno') }
        if ($ffmpeg) { $arguments += @('--ffmpeg-location', (Split-Path -Parent $ffmpeg)) }
        $arguments += 'https://raw.githubusercontent.com/yt-dlp/yt-dlp/master/LICENSE'
        # Windows PowerShell 5 wraps native stderr warnings as ErrorRecord.
        # Capture them without converting a successful yt-dlp exit into a test error.
        $previousErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            $downloadOutput = & $ytDlp @arguments 2>&1 | Out-String
            $downloadExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        Test-Requirement ($downloadExitCode -eq 0) 'tiny online yt-dlp transfer exits successfully'
        Test-Requirement ($downloadOutput -match '__YTDLP_PROGRESS__:') 'yt-dlp emits the progress marker'
        Test-Requirement (Test-Path -LiteralPath $resultFile -PathType Leaf) 'after_move creates a path result file'
        if (Test-Path -LiteralPath $resultFile -PathType Leaf) {
            $downloaded = (Get-Content -LiteralPath $resultFile -First 1).Trim()
            Test-Requirement (Test-Path -LiteralPath $downloaded -PathType Leaf) `
                'after_move returns an existing downloaded path'
        }
    } else {
        Write-Host 'SKIP tiny online yt-dlp transfer (use -OnlineYtDlp)' -ForegroundColor DarkYellow
    }

    $studyLaunchers = @(Get-ChildItem -LiteralPath $root -Filter '*.bat' -File | Where-Object {
        (Get-Content -LiteralPath $_.FullName -Raw) -match 'TetrisStudy\.exe'
    })
    Test-Requirement ($studyLaunchers.Count -ge 4) 'all dedicated BAT entry points launch TetrisStudy.exe'
} finally {
    $resolvedAudit = [IO.Path]::GetFullPath($audit)
    $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolvedAudit.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedAudit -PathType Container)) {
        Remove-Item -LiteralPath $resolvedAudit -Recurse -Force
    }
}

if ($failures.Count -gt 0) {
    throw "TetrisStudy integration audit failed: $($failures.Count) check(s)"
}
Write-Host 'All TetrisStudy pipeline checks passed.' -ForegroundColor Cyan
