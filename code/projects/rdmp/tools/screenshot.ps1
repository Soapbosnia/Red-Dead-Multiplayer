# Saves a screenshot of the primary screen (testing: what the game shows). Default: build\shots\<time>.png
#   powershell -File tools\screenshot.ps1 [name]
param([string]$Name = (Get-Date -Format 'HHmmss'))
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$dir = Join-Path $PSScriptRoot '..\build\shots'
New-Item -ItemType Directory -Force $dir | Out-Null
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$path = [IO.Path]::GetFullPath((Join-Path $dir "$Name.png"))
$bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose(); $bitmap.Dispose()
$path
