# Generate the forensic report pages for every warning match we have.
$ErrorActionPreference = 'Continue'
$mainVideo = 'C:\Users\hirom\AppData\Local\PPTLab\media\video-799b6882cb7d40f0\[yD_ZukVrgHY].mkv'

python tools\forensics_match.py --run frozen\runs\goexit-360p30-stride1-8warn `
    --data 'tools\video-recovery-runtime\data\batches\[yD_ZukVrgHY]' `
    --all --out review\forensics\main --video $mainVideo --frames 2

Get-ChildItem scratch\labsite\batches -Directory | ForEach-Object {
    $id = $_.Name
    $video = "scratch\labsite\media\$id.mp4"
    if ((Test-Path (Join-Path $_.FullName 'quality-warnings.json')) -and (Test-Path $video)) {
        "=== $id"
        python tools\forensics_match.py --run $_.FullName --all --out "review\forensics\labsite\$id" --video $video --frames 1
    }
}
'forensics-all-finished'
