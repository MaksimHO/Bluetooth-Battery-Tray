param(
    [string]$Source,
    [string]$Output
)

Add-Type -AssemblyName System.Drawing

if (-not $Source) { $Source = Join-Path $PSScriptRoot '..\resources\app-source.png' }
if (-not $Output) { $Output = Join-Path $PSScriptRoot '..\resources\app.ico' }

$code = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class IconBuilder
{
    public static void Build(string sourcePath, string outputPath, int[] sizes)
    {
        using (var source = new Bitmap(sourcePath))
        {
            var frames = new List<byte[]>();
            var widths = new List<int>();

            foreach (int size in sizes)
            {
                using (var bmp = Resize(source, size))
                {
                    frames.Add(EncodeDib(bmp));
                    widths.Add(size);
                }
            }

            using (var fs = File.Create(outputPath))
            using (var bw = new BinaryWriter(fs))
            {
                bw.Write((ushort)0);              // reserved
                bw.Write((ushort)1);              // type: icon
                bw.Write((ushort)frames.Count);   // image count

                int offset = 6 + 16 * frames.Count;
                for (int i = 0; i < frames.Count; i++)
                {
                    bw.Write((byte)(widths[i] >= 256 ? 0 : widths[i]));
                    bw.Write((byte)(widths[i] >= 256 ? 0 : widths[i]));
                    bw.Write((byte)0);            // palette colors
                    bw.Write((byte)0);            // reserved
                    bw.Write((ushort)1);          // color planes
                    bw.Write((ushort)32);         // bits per pixel
                    bw.Write((uint)frames[i].Length);
                    bw.Write((uint)offset);
                    offset += frames[i].Length;
                }

                foreach (var frame in frames)
                    bw.Write(frame);
            }
        }
    }

    static Bitmap Resize(Bitmap source, int size)
    {
        var result = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var graphics = Graphics.FromImage(result))
        {
            graphics.Clear(Color.Transparent);
            graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            graphics.CompositingQuality = CompositingQuality.HighQuality;
            graphics.SmoothingMode = SmoothingMode.HighQuality;
            graphics.DrawImage(source, new Rectangle(0, 0, size, size),
                new Rectangle(0, 0, source.Width, source.Height), GraphicsUnit.Pixel);
        }
        return result;
    }

    static byte[] EncodeDib(Bitmap bitmap)
    {
        int size = bitmap.Width;
        var data = bitmap.LockBits(new Rectangle(0, 0, size, size),
            ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);

        byte[] pixels = new byte[size * size * 4];
        try
        {
            for (int y = 0; y < size; y++)
                Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride), pixels, y * size * 4, size * 4);
        }
        finally
        {
            bitmap.UnlockBits(data);
        }

        int maskRow = ((size + 31) / 32) * 4;
        int xorSize = size * size * 4;
        int andSize = maskRow * size;

        using (var stream = new MemoryStream())
        using (var writer = new BinaryWriter(stream))
        {
            // BITMAPINFOHEADER; biHeight covers the XOR image and the AND mask.
            writer.Write(40);
            writer.Write(size);
            writer.Write(size * 2);
            writer.Write((ushort)1);
            writer.Write((ushort)32);
            writer.Write(0);                  // BI_RGB
            writer.Write(xorSize + andSize);
            writer.Write(0);
            writer.Write(0);
            writer.Write(0);
            writer.Write(0);

            // XOR pixels, bottom-up BGRA.
            for (int y = size - 1; y >= 0; y--)
                writer.Write(pixels, y * size * 4, size * 4);

            // AND mask (1 = transparent), needed by legacy icon consumers.
            var mask = new byte[andSize];
            for (int y = 0; y < size; y++)
            {
                int sourceRow = size - 1 - y;
                for (int x = 0; x < size; x++)
                {
                    if (pixels[(sourceRow * size + x) * 4 + 3] == 0)
                        mask[y * maskRow + (x / 8)] |= (byte)(0x80 >> (x % 8));
                }
            }
            writer.Write(mask, 0, mask.Length);

            return stream.ToArray();
        }
    }
}
'@

Add-Type -TypeDefinition $code -ReferencedAssemblies System.Drawing

$SourcePath = [System.IO.Path]::GetFullPath($Source)
$OutputPath = [System.IO.Path]::GetFullPath($Output)
$sizes = [int[]](16, 20, 24, 32, 40, 48, 64, 128, 256)

[IconBuilder]::Build($SourcePath, $OutputPath, $sizes)
Write-Output ("Wrote {0} ({1} bytes) from {2}" -f $OutputPath, (Get-Item $OutputPath).Length, $SourcePath)
