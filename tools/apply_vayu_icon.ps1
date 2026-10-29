param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Icon
)

$ErrorActionPreference = "Stop"
$exePath = [System.IO.Path]::GetFullPath($Executable)
$icoPath = [System.IO.Path]::GetFullPath($Icon)

if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "Executable not found: $exePath"
}
if (-not (Test-Path -LiteralPath $icoPath -PathType Leaf)) {
    throw "Icon file not found: $icoPath"
}

$source = @'
using System;
using System.IO;
using System.Runtime.InteropServices;

public static class VayuIconResource
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr BeginUpdateResourceW(string fileName, bool deleteExistingResources);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool UpdateResourceW(
        IntPtr update, IntPtr type, IntPtr name, ushort language,
        byte[] data, uint dataSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool EndUpdateResourceW(IntPtr update, bool discard);

    private static IntPtr Id(int value) { return new IntPtr(value); }

    public static void Apply(string executable, string iconFile)
    {
        byte[] ico = File.ReadAllBytes(iconFile);
        if (ico.Length < 6 || ico[0] != 0 || ico[1] != 0 || ico[2] != 1 || ico[3] != 0)
            throw new InvalidDataException("The icon file is not a valid Windows .ico file.");

        int count = ico[4] | (ico[5] << 8);
        if (count < 1 || ico.Length < 6 + count * 16)
            throw new InvalidDataException("The .ico file has an invalid image directory.");

        byte[] group = new byte[6 + count * 14];
        Buffer.BlockCopy(ico, 0, group, 0, 6);
        IntPtr update = BeginUpdateResourceW(executable, false);
        if (update == IntPtr.Zero)
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),
                "Could not open executable resources for update.");

        bool finished = false;
        try
        {
            for (int i = 0; i < count; i++)
            {
                int entry = 6 + i * 16;
                int width = ico[entry + 0];
                int height = ico[entry + 1];
                uint bytes = BitConverter.ToUInt32(ico, entry + 8);
                uint offset = BitConverter.ToUInt32(ico, entry + 12);
                int resourceId = i + 1;

                if (bytes == 0 || offset > ico.Length || bytes > ico.Length - offset)
                    throw new InvalidDataException("The .ico file contains an invalid image range.");

                byte[] image = new byte[bytes];
                Buffer.BlockCopy(ico, (int)offset, image, 0, (int)bytes);
                if (!UpdateResourceW(update, Id(3), Id(resourceId), 0, image, (uint)image.Length))
                    throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),
                        "Could not write an icon image resource.");

                int groupEntry = 6 + i * 14;
                Buffer.BlockCopy(ico, entry, group, groupEntry, 8);
                Buffer.BlockCopy(BitConverter.GetBytes(bytes), 0, group, groupEntry + 8, 4);
                Buffer.BlockCopy(BitConverter.GetBytes((ushort)resourceId), 0, group, groupEntry + 12, 2);
            }

            if (!UpdateResourceW(update, Id(14), Id(1), 0, group, (uint)group.Length))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),
                    "Could not write the executable's icon group resource.");

            if (!EndUpdateResourceW(update, false))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),
                    "Could not save the executable icon resources.");
            finished = true;
        }
        finally
        {
            if (!finished) EndUpdateResourceW(update, true);
        }
    }
}
'@

Add-Type -TypeDefinition $source
[ VayuIconResource ]::Apply($exePath, $icoPath)
Write-Host "Applied Vayu icon to: $exePath" -ForegroundColor Green
