# Copy to local-config.ps1 and replace these examples with your installed paths.
# local-config.ps1 is ignored by Git. The tested ESP-IDF version is 5.5.5.
# Use the initialization script from your ESP-IDF installer (EIM example below).
$IdfEnvironmentScript = 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'
$VideoPython = 'C:\projects\video-tools\.venv\Scripts\python.exe'
$env:VIDEO_FFMPEG = 'C:\ffmpeg\bin\ffmpeg.exe'
$env:VIDEO_FFPROBE = 'C:\ffmpeg\bin\ffprobe.exe'
