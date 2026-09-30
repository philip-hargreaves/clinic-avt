# Writes the MSIX logos in app\ClinicAVT.App\Assets from AppIcon.ico. Commit the PNGs.
#
#   make-logo-assets.ps1
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$assets = Join-Path (Split-Path $PSScriptRoot -Parent) "app\ClinicAVT.App\Assets"

# The icon's PNG images, by width
$bytes = [IO.File]::ReadAllBytes((Join-Path $assets "AppIcon.ico"))
$frames = @{}
for ($i = 0; $i -lt [BitConverter]::ToUInt16($bytes, 4); $i++) {
    $entry = 6 + 16 * $i
    $length = [BitConverter]::ToInt32($bytes, $entry + 8)
    $offset = [BitConverter]::ToInt32($bytes, $entry + 12)
    if ($bytes[$offset] -ne 0x89) { throw "AppIcon.ico image $i is not a PNG" }
    $stream = [IO.MemoryStream]::new($bytes, $offset, $length)
    $image = [Drawing.Image]::FromStream($stream)
    $frames[$image.Width] = $image
}
$largest = $frames[[int]($frames.Keys | Measure-Object -Maximum).Maximum]

function Write-Logo([string]$name, [int]$width, [int]$height, [int]$iconSize) {
    $source = if ($frames.ContainsKey($iconSize)) { $frames[$iconSize] } else { $largest }
    $canvas = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($canvas)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([Drawing.Color]::Transparent)
    $g.DrawImage($source, [int](($width - $iconSize) / 2), [int](($height - $iconSize) / 2), $iconSize, $iconSize)
    $g.Dispose()
    $canvas.Save((Join-Path $assets "$name.png"), [Drawing.Imaging.ImageFormat]::Png)
    $canvas.Dispose()
}

Get-ChildItem $assets -Filter *.png |
    Where-Object { $_.Name -match '^(Square|Wide|SplashScreen|StoreLogo)' } |
    Remove-Item

foreach ($scale in 100, 200, 400) {
    $k = $scale / 100
    Write-Logo "Square44x44Logo.scale-$scale" (44 * $k) (44 * $k) (44 * $k)
    Write-Logo "StoreLogo.scale-$scale" (50 * $k) (50 * $k) (50 * $k)
    # Tiles and splash have a margin
    Write-Logo "Square150x150Logo.scale-$scale" (150 * $k) (150 * $k) (90 * $k)
    Write-Logo "Wide310x150Logo.scale-$scale" (310 * $k) (150 * $k) (90 * $k)
    Write-Logo "SplashScreen.scale-$scale" (620 * $k) (300 * $k) (150 * $k)
}
# Taskbar sizes
foreach ($size in 16, 24, 32, 48, 256) {
    foreach ($form in "", "_altform-unplated", "_altform-lightunplated") {
        Write-Logo "Square44x44Logo.targetsize-$size$form" $size $size $size
    }
}

foreach ($image in $frames.Values) { $image.Dispose() }
Write-Host "Logo images written to $assets"
