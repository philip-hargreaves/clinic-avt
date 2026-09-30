# Writes the MSIX logo images in app\ClinicAVT.App\Assets from AppIcon.ico. Run it again when
# the icon changes, and commit the PNGs. Sizes the icon already has are copied from it. The
# other sizes are scaled from its largest image.
#
#   make-logo-assets.ps1
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$assets = Join-Path (Split-Path $PSScriptRoot -Parent) "app\ClinicAVT.App\Assets"

# Read the images inside the icon, keyed by width. The entries start 6 bytes in and are 16
# bytes each, with the image's length and offset at bytes 8 and 12 of the entry. Every image
# must be a PNG
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

# Draws the icon, iconSize pixels square, in the centre of a transparent image
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
    # App list, taskbar and Start, then the logo the installer shows
    Write-Logo "Square44x44Logo.scale-$scale" (44 * $k) (44 * $k) (44 * $k)
    Write-Logo "StoreLogo.scale-$scale" (50 * $k) (50 * $k) (50 * $k)
    # Tiles and splash keep a margin round the icon
    Write-Logo "Square150x150Logo.scale-$scale" (150 * $k) (150 * $k) (90 * $k)
    Write-Logo "Wide310x150Logo.scale-$scale" (310 * $k) (150 * $k) (90 * $k)
    Write-Logo "SplashScreen.scale-$scale" (620 * $k) (300 * $k) (150 * $k)
}
# Taskbar and title bar sizes, with the unplated versions Windows uses on light and dark taskbars
foreach ($size in 16, 24, 32, 48, 256) {
    foreach ($form in "", "_altform-unplated", "_altform-lightunplated") {
        Write-Logo "Square44x44Logo.targetsize-$size$form" $size $size $size
    }
}

foreach ($image in $frames.Values) { $image.Dispose() }
Write-Host "Logo images written to $assets"
