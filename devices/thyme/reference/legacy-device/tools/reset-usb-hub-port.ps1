param(
  [string]$InstanceMatch = "VID_05E3&PID_0610",
  [int]$Port = 2
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class HakiosUsbPortReset
{
    private static Guid HubInterface = new Guid("f18a0e88-c30c-11d0-8815-00a0c906bed8");
    private const int CR_SUCCESS = 0;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ_WRITE = 0x00000003;
    private const uint OPEN_EXISTING = 3;
    private const uint IOCTL_USB_RESET_PORT = 0x00220410;
    private static readonly IntPtr InvalidHandle = new IntPtr(-1);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    private static extern int CM_Get_Device_Interface_List_Size(
        out int length, ref Guid interfaceClassGuid, string deviceID, int flags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    private static extern int CM_Get_Device_Interface_List(
        ref Guid interfaceClassGuid, string deviceID, StringBuilder buffer, int length, int flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFile(
        string name, uint access, uint share, IntPtr security, uint disposition,
        uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool DeviceIoControl(
        IntPtr device, uint code, byte[] input, int inputSize, byte[] output,
        int outputSize, out int returned, IntPtr overlapped);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    public static string Reset(string instanceMatch, int port)
    {
        int length = 0;
        int result = CM_Get_Device_Interface_List_Size(
            out length, ref HubInterface, null, 0);
        if (result != CR_SUCCESS || length <= 1)
            throw new Exception("cannot enumerate USB hub interfaces: " + result);

        int capacity = Math.Max(length * 4, 65536);
        var buffer = new StringBuilder(capacity);
        result = CM_Get_Device_Interface_List(
            ref HubInterface, null, buffer, capacity, 0);
        if (result != CR_SUCCESS)
            throw new Exception("cannot read USB hub interfaces: " + result);

        string selected = null;
        foreach (string item in buffer.ToString().Split(new char[] { '\0' },
            StringSplitOptions.RemoveEmptyEntries)) {
            if (item.IndexOf(instanceMatch, StringComparison.OrdinalIgnoreCase) >= 0) {
                selected = item;
                break;
            }
        }
        if (selected == null)
            throw new Exception("matching USB hub interface was not found");

        IntPtr handle = CreateFile(selected, GENERIC_WRITE, FILE_SHARE_READ_WRITE,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (handle == InvalidHandle)
            throw new Exception("CreateFile failed: " + Marshal.GetLastWin32Error());

        try {
            var request = new byte[64];
            BitConverter.GetBytes(port).CopyTo(request, 0);
            int returned;
            if (!DeviceIoControl(handle, IOCTL_USB_RESET_PORT, request,
                request.Length, null, 0, out returned, IntPtr.Zero))
                throw new Exception("IOCTL_USB_RESET_PORT failed: " +
                    Marshal.GetLastWin32Error());
            return selected;
        } finally {
            CloseHandle(handle);
        }
    }
}
"@

$path = [HakiosUsbPortReset]::Reset($InstanceMatch, $Port)
Write-Output "reset USB hub port $Port via $path"
