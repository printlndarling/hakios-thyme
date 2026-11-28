# Run on native Windows while thyme is in the private RAM Boot USB test mode.
param(
    [ValidateRange(5, 360)] [int] $WaitSeconds = 90,
    [ValidateRange(1, 6)] [int] $Rounds = 3
)
$ErrorActionPreference = 'Stop'

if (-not ('ThymeUsbLoopback' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;

public static class ThymeUsbLoopback
{
    private static Guid InterfaceGuid = new Guid("A4083620-32F7-5F01-8504-C2BEFA7DF2A7");

    [StructLayout(LayoutKind.Sequential)]
    private struct DeviceInterface
    {
        public uint Size;
        public Guid InterfaceClass;
        public uint Flags;
        public UIntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Pipe
    {
        public int Type;
        public byte Id;
        public ushort Packet;
        public byte Interval;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevs(ref Guid guid, IntPtr enumerator, IntPtr parent, uint flags);
    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiEnumDeviceInterfaces(IntPtr devices, IntPtr info, ref Guid guid,
        uint index, ref DeviceInterface data);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr devices, ref DeviceInterface data,
        IntPtr detail, uint size, out uint required, IntPtr info);
    [DllImport("setupapi.dll")]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr devices);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("winusb.dll", SetLastError = true)]
    private static extern bool WinUsb_Initialize(SafeFileHandle file, out IntPtr handle);
    [DllImport("winusb.dll")]
    private static extern bool WinUsb_Free(IntPtr handle);
    [DllImport("winusb.dll", SetLastError = true)]
    private static extern bool WinUsb_QueryPipe(IntPtr handle, byte setting, byte index, out Pipe pipe);
    [DllImport("winusb.dll", SetLastError = true)]
    private static extern bool WinUsb_SetPipePolicy(IntPtr handle, byte pipe, uint policy, uint size, ref uint value);
    [DllImport("winusb.dll", SetLastError = true)]
    private static extern bool WinUsb_WritePipe(IntPtr handle, byte pipe, byte[] bytes, uint length,
        out uint transferred, IntPtr overlapped);
    [DllImport("winusb.dll", SetLastError = true)]
    private static extern bool WinUsb_ReadPipe(IntPtr handle, byte pipe, [Out] byte[] bytes, uint length,
        out uint transferred, IntPtr overlapped);

    private static void Check(bool ok, string operation)
    {
        if (!ok)
        {
            int error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, operation + " failed with Win32 error " + error);
        }
    }

    public static string Find()
    {
        IntPtr devices = SetupDiGetClassDevs(ref InterfaceGuid, IntPtr.Zero, IntPtr.Zero, 0x12);
        Check(devices != new IntPtr(-1), "SetupDiGetClassDevs");
        string found = null;
        try
        {
            for (uint index = 0;; index++)
            {
                var data = new DeviceInterface { Size = (uint)Marshal.SizeOf(typeof(DeviceInterface)) };
                if (!SetupDiEnumDeviceInterfaces(devices, IntPtr.Zero, ref InterfaceGuid, index, ref data))
                {
                    if (Marshal.GetLastWin32Error() == 259) break;
                    Check(false, "SetupDiEnumDeviceInterfaces");
                }
                uint required;
                SetupDiGetDeviceInterfaceDetail(devices, ref data, IntPtr.Zero, 0, out required, IntPtr.Zero);
                if (required < 8) Check(false, "Windows interface detail size");
                IntPtr detail = Marshal.AllocHGlobal((int)required);
                try
                {
                    Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
                    Check(SetupDiGetDeviceInterfaceDetail(devices, ref data, detail, required, out required,
                        IntPtr.Zero), "SetupDiGetDeviceInterfaceDetail");
                    string path = Marshal.PtrToStringUni(IntPtr.Add(detail, 4));
                    if (path.IndexOf("vid_1209&pid_0001", StringComparison.OrdinalIgnoreCase) < 0) continue;
                    if (found != null) throw new IOException("More than one HakiOS loopback test interface");
                    found = path;
                }
                finally
                {
                    Marshal.FreeHGlobal(detail);
                }
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(devices);
        }
        return found;
    }

    public static string Run(string path, int seed)
    {
        using (SafeFileHandle file = CreateFile(path, 0xc0000000, 3, IntPtr.Zero, 3, 0x40000000, IntPtr.Zero))
        {
            Check(!file.IsInvalid, "CreateFile for the HakiOS WinUSB interface");
            IntPtr handle;
            Check(WinUsb_Initialize(file, out handle), "WinUsb_Initialize");
            try
            {
                Pipe output = new Pipe(), input = new Pipe();
                for (byte index = 0; index < 2; index++)
                {
                    Pipe pipe;
                    Check(WinUsb_QueryPipe(handle, 0, index, out pipe), "WinUsb_QueryPipe");
                    if (pipe.Type != 2) throw new IOException("Expected a bulk pipe");
                    if (pipe.Id == 0x01) output = pipe;
                    if (pipe.Id == 0x81) input = pipe;
                }
                int packet = output.Packet;
                if ((packet != 64 && packet != 512) || input.Packet != packet)
                    throw new IOException("Unexpected bulk endpoint addresses or packet sizes");
                uint timeout = 3000;
                Check(WinUsb_SetPipePolicy(handle, 0x01, 3, 4, ref timeout), "OUT transfer timeout");
                Check(WinUsb_SetPipePolicy(handle, 0x81, 3, 4, ref timeout), "IN transfer timeout");

                var random = new Random(seed);
                var received = new byte[packet];
                var lengths = new int[] { 1, 7, 31, 63, 64, 65, 127, 128, 255, 256, 511, 512 };
                int total = 0;
                using (var outputBytes = new MemoryStream())
                {
                    for (int i = 0; i < 256; i++)
                    {
                        int length = i < lengths.Length ? Math.Min(lengths[i], packet) :
                            (i % 4 == 0 ? random.Next(1, packet + 1) : packet);
                        var sent = new byte[length];
                        random.NextBytes(sent);
                        uint count;
                        Check(WinUsb_WritePipe(handle, 0x01, sent, (uint)length, out count, IntPtr.Zero),
                            "Bulk OUT transfer " + (i + 1) + " length " + length);
                        if (count != length) throw new IOException("Short bulk OUT write at transfer " + (i + 1));
                        Check(WinUsb_ReadPipe(handle, 0x81, received, (uint)length, out count, IntPtr.Zero),
                            "Bulk IN transfer " + (i + 1) + " length " + length);
                        if (count != length) throw new IOException("Short bulk IN read at transfer " + (i + 1));
                        for (int offset = 0; offset < length; offset++)
                            if (received[offset] != sent[offset])
                                throw new IOException("Bulk data mismatch at transfer " + (i + 1) + ", byte " + offset);
                        outputBytes.Write(received, 0, length);
                        total += length;
                    }
                    outputBytes.Position = 0;
                    using (var sha = SHA256.Create())
                    {
                        string hash = BitConverter.ToString(sha.ComputeHash(outputBytes)).Replace("-", "").ToLowerInvariant();
                        return "packet=" + packet + " transfers=256 bytes_each_direction=" + total + " sha256=" + hash;
                    }
                }
            }
            finally
            {
                WinUsb_Free(handle);
            }
        }
    }
}
'@
}

$deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
do {
    $path = [ThymeUsbLoopback]::Find()
    if ($null -ne $path) { break }
    Start-Sleep -Milliseconds 800
} while ([DateTime]::UtcNow -lt $deadline)
if ($null -eq $path) { throw 'The HakiOS WinUSB test interface did not appear' }

for ($round = 1; $round -le $Rounds; $round++) {
    $path = [ThymeUsbLoopback]::Find()
    if ($null -eq $path) { throw 'The WinUSB interface disappeared between rounds' }
    "$(Get-Date -Format o) round=$round $([ThymeUsbLoopback]::Run($path, 20261128 + $round))"
}
