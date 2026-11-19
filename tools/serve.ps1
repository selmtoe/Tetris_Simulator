param(
    [ValidateRange(1024, 65535)]
    [int]$Port = 8765,
    [string]$OpenPath = 'hub/',
    [switch]$NoBrowser
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$rootPrefix = $repositoryRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
$mimeTypes = @{
    '.css' = 'text/css; charset=utf-8'
    '.gif' = 'image/gif'
    '.html' = 'text/html; charset=utf-8'
    '.ico' = 'image/x-icon'
    '.jpeg' = 'image/jpeg'
    '.jpg' = 'image/jpeg'
    '.js' = 'text/javascript; charset=utf-8'
    '.json' = 'application/json; charset=utf-8'
    '.map' = 'application/json; charset=utf-8'
    '.onnx' = 'application/octet-stream'
    '.png' = 'image/png'
    '.svg' = 'image/svg+xml'
    '.txt' = 'text/plain; charset=utf-8'
    '.webmanifest' = 'application/manifest+json; charset=utf-8'
    '.wasm' = 'application/wasm'
}

function Start-StaticListener {
    param([int]$StartPort)

    for ($candidatePort = $StartPort; $candidatePort -lt ($StartPort + 20); $candidatePort++) {
        $candidate = $null
        try {
            $candidate = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $candidatePort)
            $candidate.Start()
            return @{ Listener = $candidate; Port = $candidatePort }
        } catch {
            if ($null -ne $candidate) { $candidate.Stop() }
        }
    }

    throw "Could not bind a local server port between $StartPort and $($StartPort + 19)."
}

function Write-Headers {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [int]$StatusCode,
        [string]$Reason,
        [string]$ContentType,
        [Int64]$Length
    )

    $headers = "HTTP/1.1 $StatusCode $Reason`r`nContent-Type: $ContentType`r`nContent-Length: $Length`r`nCache-Control: no-cache`r`nConnection: close`r`n`r`n"
    $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($headers)
    $Stream.Write($headerBytes, 0, $headerBytes.Length)
}

function Send-TextResponse {
    param(
        [System.Net.Sockets.NetworkStream]$Stream,
        [int]$StatusCode,
        [string]$Reason,
        [string]$Message
    )

    $body = [System.Text.Encoding]::UTF8.GetBytes($Message)
    Write-Headers -Stream $Stream -StatusCode $StatusCode -Reason $Reason -ContentType 'text/plain; charset=utf-8' -Length $body.Length
    $Stream.Write($body, 0, $body.Length)
}

$server = Start-StaticListener -StartPort $Port
$listener = $server.Listener
$baseUrl = "http://127.0.0.1:$($server.Port)/"
$initialUrl = $baseUrl + $OpenPath.TrimStart('/')

Write-Host "Tetris AI is running at $baseUrl"
Write-Host 'Press Ctrl+C to stop the local server.'
if (-not $NoBrowser) {
    Start-Process $initialUrl
}

try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        $stream = $null
        $reader = $null
        try {
            $client.ReceiveTimeout = 5000
            $client.SendTimeout = 5000
            $stream = $client.GetStream()
            $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::ASCII, $false, 4096, $true)
            $requestLine = $reader.ReadLine()

            while ($true) {
                $headerLine = $reader.ReadLine()
                if ([string]::IsNullOrEmpty($headerLine)) { break }
            }

            if ($requestLine -notmatch '^(GET|HEAD)\s+([^\s]+)\s+HTTP/\d\.\d$') {
                Send-TextResponse -Stream $stream -StatusCode 405 -Reason 'Method Not Allowed' -Message 'Only GET and HEAD are supported.'
                continue
            }

            $method = $Matches[1]
            $requestTarget = $Matches[2]
            try {
                $uri = [System.Uri]::new("http://127.0.0.1$requestTarget")
                $relativePath = [System.Uri]::UnescapeDataString($uri.AbsolutePath).TrimStart('/').Replace('/', [System.IO.Path]::DirectorySeparatorChar)
                if ($relativePath -eq '.archive' -or $relativePath.StartsWith(".archive$([System.IO.Path]::DirectorySeparatorChar)", [System.StringComparison]::OrdinalIgnoreCase)) {
                    Send-TextResponse -Stream $stream -StatusCode 403 -Reason 'Forbidden' -Message 'Forbidden.'
                    continue
                }
                $candidatePath = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $relativePath))
            } catch {
                Send-TextResponse -Stream $stream -StatusCode 400 -Reason 'Bad Request' -Message 'Invalid request path.'
                continue
            }

            if (($candidatePath -ne $repositoryRoot) -and -not $candidatePath.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                Send-TextResponse -Stream $stream -StatusCode 403 -Reason 'Forbidden' -Message 'Forbidden.'
                continue
            }

            if ([System.IO.Directory]::Exists($candidatePath)) {
                $candidatePath = Join-Path $candidatePath 'index.html'
            }

            if (-not [System.IO.File]::Exists($candidatePath)) {
                Send-TextResponse -Stream $stream -StatusCode 404 -Reason 'Not Found' -Message 'Not found.'
                continue
            }

            $extension = [System.IO.Path]::GetExtension($candidatePath).ToLowerInvariant()
            $contentType = if ($mimeTypes.ContainsKey($extension)) { $mimeTypes[$extension] } else { 'application/octet-stream' }
            $fileInfo = [System.IO.FileInfo]$candidatePath
            Write-Headers -Stream $stream -StatusCode 200 -Reason 'OK' -ContentType $contentType -Length $fileInfo.Length

            if ($method -eq 'GET') {
                $fileStream = [System.IO.File]::OpenRead($candidatePath)
                try {
                    $fileStream.CopyTo($stream)
                } finally {
                    $fileStream.Dispose()
                }
            }
        } catch {
            try { Send-TextResponse -Stream $stream -StatusCode 500 -Reason 'Internal Server Error' -Message 'Unexpected local server error.' } catch { }
        } finally {
            if ($null -ne $reader) { $reader.Dispose() }
            if ($null -ne $stream) { $stream.Dispose() }
            $client.Close()
        }
    }
} finally {
    $listener.Stop()
}
