# Run on native Windows while thyme is in the private RAM Boot USB test mode.
param(
    [ValidateRange(5, 360)] [int] $WaitSeconds = 90,
    [ValidateRange(1, 6)] [int] $Rounds = 3,
    [switch] $UsbMux,
    [switch] $BridgeStream,
    [ValidateRange(0, 65535)] [int] $BridgePort,
    [ValidatePattern("^[a-f0-9]{64}$")] [string] $BridgeLocation
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
    private const int LockdownMessageMax = 32768;
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
    [DllImport("winusb.dll", SetLastError = true)]
    private static extern bool WinUsb_GetDescriptor(IntPtr handle, byte type, byte index, ushort language,
        [Out] byte[] buffer, uint length, out uint transferred);
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

    private static void Send(IntPtr handle, byte[] packet)
    {
        uint count;
        Check(WinUsb_WritePipe(handle, 0x01, packet, (uint)packet.Length, out count, IntPtr.Zero),
            "usbmux bulk OUT");
        if (count != packet.Length) throw new IOException("Short usbmux write");
    }

    private static void Expect(IntPtr handle, byte[] expected)
    {
        var packet = new byte[expected.Length];
        uint count;
        Check(WinUsb_ReadPipe(handle, 0x81, packet, (uint)packet.Length, out count, IntPtr.Zero),
            "usbmux bulk IN");
        if (count != expected.Length) throw new IOException("Unexpected usbmux response length " + count);
        for (int i = 0; i < expected.Length; i++)
            if (packet[i] != expected[i]) throw new IOException("usbmux response differs at byte " + i);
    }

    private static readonly byte[] VersionRequest = {
        0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0
    };
    private static readonly byte[] VersionReply = {
        0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0
    };

    private static byte[] ConnectRequest(int port)
    {
        return new byte[] {
            0, 0, 0, 6, 0, 0, 0, 28, 0xc0, 0, (byte)(port >> 8), (byte)port,
            1, 0x23, 0x45, 0x67, 0, 0, 0, 0, 0x50, 2, 0x02, 0, 0, 0, 0, 0
        };
    }

    private static byte[] ConnectRefused(int port)
    {
        return new byte[] {
            0, 0, 0, 6, 0, 0, 0, 28, (byte)(port >> 8), (byte)port, 0xc0, 0,
            0, 0, 0, 0, 1, 0x23, 0x45, 0x68, 0x50, 0x14, 0, 0, 0, 0, 0, 0
        };
    }

    private static string TestUsbMux(IntPtr handle)
    {
        byte[] malformed = { 0, 0, 0, 0, 0, 0, 0, 7 };
        byte[] propose = VersionRequest, version = VersionReply;
        byte[] syn = ConnectRequest(62079), reset = ConnectRefused(62079);
        Send(handle, malformed);
        var first = new byte[5];
        var rest = new byte[15];
        Array.Copy(propose, first, first.Length);
        Array.Copy(propose, first.Length, rest, 0, rest.Length);
        Send(handle, first);
        Send(handle, rest);
        Expect(handle, version);
        Send(handle, syn);
        Expect(handle, reset);

        // A single USB write can carry more than one frame.
        var combined = new byte[propose.Length + syn.Length];
        Array.Copy(propose, combined, propose.Length);
        Array.Copy(syn, 0, combined, propose.Length, syn.Length);
        Send(handle, combined);
        Expect(handle, version);
        Expect(handle, reset);
        return "usbmux=1.0 fragmented=pass combined=pass malformed_recovery=pass unserved_port=closed";
    }

    private static string Location(string path)
    {
        using (var sha = SHA256.Create())
            return BitConverter.ToString(sha.ComputeHash(System.Text.Encoding.UTF8.GetBytes(path.ToLowerInvariant())))
                .Replace("-", "").ToLowerInvariant();
    }

    private static bool Negotiate(IntPtr handle)
    {
        var descriptor = new byte[18];
        uint count;
        Check(WinUsb_GetDescriptor(handle, 1, 0, 0, descriptor, 18, out count), "USB device descriptor");
        if (count != 18 || descriptor[8] != 9 || descriptor[9] != 0x12 || descriptor[10] != 1 ||
            descriptor[11] != 0 || descriptor[12] != 3 || descriptor[13] != 1)
            return false; // The separate loopback profile uses revision 0102.
        Send(handle, VersionRequest);
        Expect(handle, VersionReply);
        return true;
    }

    public static string Probe(string path, int port, string expectedLocation)
    {
        string location = Location(path);
        if (!String.IsNullOrEmpty(expectedLocation) && location != expectedLocation)
            return null;
        return WithPipes(path, (handle, packet) =>
        {
            if (!Negotiate(handle)) return null;
            if (port != 0)
            {
                Send(handle, ConnectRequest(port));
                Expect(handle, ConnectRefused(port));
            }
            return location;
        });
    }

    private static uint Read32(byte[] bytes, int offset)
    {
        return ((uint)bytes[offset] << 24) | ((uint)bytes[offset + 1] << 16) |
            ((uint)bytes[offset + 2] << 8) | bytes[offset + 3];
    }

    private static void Write32(byte[] bytes, int offset, uint value)
    {
        for (int i = 3; i >= 0; --i)
        {
            bytes[offset + i] = (byte)value;
            value >>= 8;
        }
    }

    private static byte[] ReadBulk(IntPtr handle, int length)
    {
        var bytes = new byte[length];
        uint count;
        Check(WinUsb_ReadPipe(handle, 0x81, bytes, (uint)length, out count, IntPtr.Zero), "usbmux bulk IN");
        Array.Resize(ref bytes, (int)count);
        return bytes;
    }

    private static byte[] ReceiveTcp(IntPtr handle, int maxPacket, int port)
    {
        byte[] frame = ReadBulk(handle, maxPacket);
        if (frame.Length < 28 || Read32(frame, 0) != 6 || frame[8] != (port >> 8) ||
            frame[9] != (byte)port || frame[10] != 0xc0 || frame[11] != 0 || frame[20] != 0x50)
            throw new IOException("Unexpected usbmux TCP header");
        uint length = Read32(frame, 4);
        if (length < frame.Length || length > 508)
            throw new IOException("Unexpected usbmux TCP length");
        if (length > frame.Length)
        {
            int first = frame.Length;
            byte[] rest = ReadBulk(handle, (int)length - first);
            if (rest.Length != length - first) throw new IOException("Incomplete usbmux TCP frame");
            Array.Resize(ref frame, (int)length);
            Array.Copy(rest, 0, frame, first, rest.Length);
        }
        return frame;
    }

    private static void SendTcp(IntPtr handle, int port, uint seq, uint ack, byte flags,
        byte[] data, int offset, int length)
    {
        var frame = new byte[28 + length];
        Array.Copy(ConnectRequest(port), frame, 28);
        Write32(frame, 4, (uint)frame.Length);
        Write32(frame, 12, seq);
        Write32(frame, 16, ack);
        frame[21] = flags;
        if (length > 0) Array.Copy(data, offset, frame, 28, length);
        Send(handle, frame);
    }

    public static void Stream(string path, int port, string expectedLocation)
    {
        if (path == null || Location(path) != expectedLocation)
        {
            Console.WriteLine("missing");
            return;
        }
        WithPipes(path, (handle, packet) =>
        {
            if (!Negotiate(handle))
            {
                Console.WriteLine("missing");
                return null;
            }
            Send(handle, ConnectRequest(port));
            byte[] reply = ReceiveTcp(handle, packet, port);
            uint seq = 0x01234568;
            if (reply.Length != 28 || Read32(reply, 16) != seq)
                throw new IOException("Unexpected usbmux connection reply");
            if (reply[21] == 0x14)
            {
                Console.WriteLine("refused");
                return null;
            }
            if (port != 62078 || reply[21] != 0x12)
                throw new IOException("Unexpected usbmux SYN/ACK");
            uint ack = unchecked(Read32(reply, 12) + 1);
            SendTcp(handle, port, seq, ack, 0x10, null, 0, 0);
            Console.WriteLine("connected");
            try
            {
                for (string line; (line = Console.ReadLine()) != null;)
                {
                    byte[] request = Convert.FromBase64String(line);
                    if (request.Length <= 4 || request.Length > LockdownMessageMax || Read32(request, 0) != request.Length - 4)
                        throw new IOException("Invalid Lockdown request size");
                    for (int offset = 0; offset < request.Length;)
                    {
                        int size = Math.Min(request.Length - offset, packet - 28);
                        SendTcp(handle, port, seq, ack, 0x18, request, offset, size);
                        seq = unchecked(seq + (uint)size);
                        offset += size;
                        reply = ReceiveTcp(handle, packet, port);
                        if (offset < request.Length &&
                            (reply.Length != 28 || reply[21] != 0x10 ||
                             Read32(reply, 12) != ack || Read32(reply, 16) != seq))
                            throw new IOException("Unexpected acknowledgement of the Lockdown request");
                    }
                    if (reply.Length <= 32) throw new IOException("Missing framed Lockdown response");
                    uint length = Read32(reply, 28);
                    if (length == 0 || length > LockdownMessageMax - 4)
                        throw new IOException("Invalid Lockdown response size");
                    var response = new byte[4 + (int)length];
                    for (int offset = 0;;)
                    {
                        int size = reply.Length - 28;
                        if (size <= 0 || size > response.Length - offset ||
                            Read32(reply, 12) != ack || Read32(reply, 16) != seq)
                            throw new IOException("Unexpected Lockdown response segment");
                        Array.Copy(reply, 28, response, offset, size);
                        offset += size;
                        ack = unchecked(ack + (uint)size);
                        if (offset == response.Length)
                        {
                            if (reply[21] != 0x18) throw new IOException("Missing final Lockdown response flag");
                            break;
                        }
                        if (reply[21] != 0x10) throw new IOException("Incomplete Lockdown response");
                        SendTcp(handle, port, seq, ack, 0x10, null, 0, 0);
                        reply = ReceiveTcp(handle, packet, port);
                    }
                    Console.WriteLine(Convert.ToBase64String(response));
                }
            }
            finally
            {
                SendTcp(handle, port, seq, ack, 0x04, null, 0, 0);
            }
            return null;
        });
    }

    private static string WithPipes(string path, Func<IntPtr, int, string> action)
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

                return action(handle, packet);
            }
            finally
            {
                WinUsb_Free(handle);
            }
        }
    }

    public static string Run(string path, int seed, bool usbMux)
    {
        return WithPipes(path, (handle, packet) =>
        {
            if (usbMux) return TestUsbMux(handle);
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
        });
    }
}
'@
}

if ($BridgeStream) {
    if (!$BridgePort -or !$BridgeLocation) { throw 'A port and matching WinUSB location are required' }
    [ThymeUsbLoopback]::Stream([ThymeUsbLoopback]::Find(), $BridgePort, $BridgeLocation)
    return
}

if ($PSBoundParameters.ContainsKey('BridgePort')) {
    $device = [ThymeUsbLoopback]::Find()
    if ($null -ne $device) { [ThymeUsbLoopback]::Probe($device, $BridgePort, $BridgeLocation) }
    return
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
    "$(Get-Date -Format o) round=$round $([ThymeUsbLoopback]::Run($path, 20261128 + $round, $UsbMux.IsPresent))"
}
