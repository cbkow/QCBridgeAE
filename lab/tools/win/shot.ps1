# Screenshot of the whole virtual desktop, run in the interactive session.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
$out = if ($args.Count -gt 0) { $args[0] } else { "C:\qcb-lab\shot.png" }
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
