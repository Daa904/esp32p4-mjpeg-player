param(
    [Parameter(Position = 0)]
    [string]$Video,
    [Parameter(Mandatory = $true, Position = 1)]
    [ValidateRange(1, 2147483647)]
    [int]$Seconds,
    [Parameter(Position = 2)]
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port = 'COM7',
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$projectPath = Join-Path $PSScriptRoot 'project\flash_video_mvp'
$videoFolder = Join-Path $PSScriptRoot 'mp4'
$shellPath = Join-Path $PSHOME 'powershell.exe'
if (-not (Test-Path -LiteralPath $shellPath)) { $shellPath = Join-Path $PSHOME 'pwsh.exe' }

try {
    $localConfig = Join-Path $PSScriptRoot 'local-config.ps1'
    if (-not (Test-Path -LiteralPath $localConfig)) {
        throw '请先复制 local-config.example.ps1 为 local-config.ps1，并填写本机工具路径。'
    }
    . $localConfig
    $pythonPath = $VideoPython
    if (-not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) {
        throw '找不到转码 Python，请检查 local-config.ps1 中的 VideoPython。'
    }
    if ([string]::IsNullOrWhiteSpace($Video)) {
        $videos = @(Get-ChildItem -LiteralPath $videoFolder -File | Where-Object { $_.Extension -ieq '.mp4' })
        if ($videos.Count -ne 1) {
            throw "MP4 文件夹中必须只有一个视频，或用 -Video 指定文件名。当前文件：$($videos.Name -join ', ')"
        }
        $sourcePath = $videos[0].FullName
    } else {
        $sourcePath = Join-Path $videoFolder $Video
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { throw "找不到视频：$sourcePath" }
        $sourcePath = (Resolve-Path -LiteralPath $sourcePath).Path
    }
    if (-not $BuildOnly -and $Port -notin [System.IO.Ports.SerialPort]::GetPortNames()) {
        throw "找不到串口 $Port，请连接下载接口或用 -Port 指定正确串口。"
    }
    $env:PYTHONUTF8 = '1'
    Write-Host "[1/3] 转码：$sourcePath，前 $Seconds 秒。"
    & $pythonPath (Join-Path $projectPath 'tools\prepare_video.py') --video $sourcePath --seconds $Seconds
    if ($LASTEXITCODE -ne 0) { throw '转码或容量检查未通过，已停止；不会烧录。' }

    Push-Location $projectPath
    try {
        # Always select autoplay for this workflow, even after manual diagnostic testing.
        $configPath = Join-Path $projectPath 'sdkconfig'
        if (Test-Path -LiteralPath $configPath) {
            $configText = [System.IO.File]::ReadAllText($configPath)
            $configText = $configText.Replace('CONFIG_MVP_DIAGNOSTIC_BOOT=y', '# CONFIG_MVP_DIAGNOSTIC_BOOT is not set')
            [System.IO.File]::WriteAllText($configPath, $configText, [System.Text.UTF8Encoding]::new($false))
        }
        Write-Host '[2/3] 编译并检查程序与视频分区容量……'
        $buildLog = Join-Path $projectPath 'logs\workflow_build.log'
        # Windows PowerShell 5 treats redirected native stderr as error records.
        # Decide success from the exit code, not from warnings written to stderr.
        $ErrorActionPreference = 'Continue'
        try {
            & $shellPath -NoProfile -ExecutionPolicy Bypass -File '.\tools\idf.ps1' build *> $buildLog
            $buildExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = 'Stop'
        }
        if ($buildExit -ne 0) {
            if (Select-String -LiteralPath $buildLog -Pattern 'too large|does not fit|overflowed' -Quiet) {
                throw "程序或视频文件过大：超过对应分区容量。未烧录。日志：$buildLog"
            }
            Get-Content -LiteralPath $buildLog -Tail 25 | Write-Host
            throw "编译失败，未烧录。日志：$buildLog"
        }
        $report = Get-Content '.\logs\video_validation.json' -Raw | ConvertFrom-Json
        $firmwareBytes = (Get-Item '.\build\flash_video_mvp.bin').Length
        Write-Host "编译通过：$($report.frames) 帧，$($report.fps) fps，播放器 $firmwareBytes 字节，独立视频 $($report.clip_bytes) 字节。"
        if ($BuildOnly) {
            Write-Host '已完成转码和编译（BuildOnly），未连接或烧录开发板。'
        } else {
            Write-Host "[3/3] 烧录到 $Port；请确保串口监视器已退出。"
            & $shellPath -NoProfile -ExecutionPolicy Bypass -File '.\tools\idf.ps1' -p $Port flash
            if ($LASTEXITCODE -ne 0) { throw '烧录失败。请检查数据线、端口及串口占用；不能确认新视频已写入。' }
            Write-Host '烧录成功，已请求开发板复位；程序将自动循环播放，无需输入 n/p。请观察屏幕验证。'
        }
    } finally {
        Pop-Location
    }
    exit 0
} catch {
    Write-Host "错误：$($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
