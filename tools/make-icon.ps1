# Generates src\icon.ico (PNG-compressed multi-size icon). Run once; the .ico is committed.
Add-Type -AssemblyName System.Drawing

$sizes = 256, 128, 64, 48, 32, 16
$pngs = @()

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    # rounded dark plate
    $r = [Math]::Max(2, [int]($s * 0.20))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($s - $d, 0, $d, $d, 270, 90)
    $path.AddArc($s - $d, $s - $d, $d, $d, 0, 90)
    $path.AddArc(0, $s - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $plate = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 20, 20, 24))
    $g.FillPath($plate, $path)

    # amber spectrum bars
    $amber = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 251, 191, 36))
    $heights = @(0.34, 0.62, 0.94, 0.50, 0.72)
    $n = $heights.Count
    $inset = $s * 0.20
    $usable = $s - $inset * 2
    $bw = $usable / ($n * 1.8)
    $gap = ($usable - $bw * $n) / ($n - 1)
    $baseY = $s - $inset

    for ($i = 0; $i -lt $n; $i++) {
        $x = $inset + $i * ($bw + $gap)
        $h = $usable * $heights[$i]
        $g.FillRectangle($amber, [float]$x, [float]($baseY - $h), [float]$bw, [float]$h)
    }

    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , $ms.ToArray()
    $bmp.Dispose()
    $ms.Dispose()
}

# assemble ICO container
$out = New-Object System.IO.MemoryStream
$bw2 = New-Object System.IO.BinaryWriter($out)
$bw2.Write([UInt16]0); $bw2.Write([UInt16]1); $bw2.Write([UInt16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $bw2.Write([Byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw2.Write([Byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw2.Write([Byte]0); $bw2.Write([Byte]0)
    $bw2.Write([UInt16]1); $bw2.Write([UInt16]32)
    $bw2.Write([UInt32]$pngs[$i].Length)
    $bw2.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw2.Write($p) }
$bw2.Flush()

$dest = Join-Path (Split-Path -Parent $PSScriptRoot) "src\icon.ico"
[System.IO.File]::WriteAllBytes($dest, $out.ToArray())
$bw2.Dispose(); $out.Dispose()
Write-Host "wrote $dest ($((Get-Item $dest).Length) bytes)"
