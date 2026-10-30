#requires -Version 7
# Read-only: attaches to a process console, copies the screen buffer, detaches.
param([Parameter(Mandatory)][int]$ProcessId, [int]$Lines = 40)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ConsoleReader {
    [StructLayout(LayoutKind.Sequential)] struct COORD { public short X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct SMALL_RECT { public short Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct CSBI { public COORD Size, Cursor; public short Attr; public SMALL_RECT Window; public COORD Max; }
    [DllImport("kernel32.dll")] static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetConsoleScreenBufferInfo(IntPtr h, out CSBI info);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ReadConsoleOutputCharacterW(IntPtr h, StringBuilder buf, uint len, COORD at, out uint read);
    public static string[] Read(uint pid, int lines) {
        FreeConsole();
        if (!AttachConsole(pid)) throw new Exception("AttachConsole failed: " + Marshal.GetLastWin32Error());
        IntPtr h = CreateFileW("CONOUT$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        try {
            CSBI info;
            if (!GetConsoleScreenBufferInfo(h, out info)) throw new Exception("GetConsoleScreenBufferInfo failed: " + Marshal.GetLastWin32Error());
            int width = info.Size.X, last = info.Cursor.Y, first = Math.Max(0, last - lines + 1);
            var result = new string[last - first + 1];
            for (int y = first; y <= last; y++) {
                var sb = new StringBuilder(width); uint read;
                ReadConsoleOutputCharacterW(h, sb, (uint)width, new COORD { X = 0, Y = (short)y }, out read);
                result[y - first] = sb.ToString().TrimEnd();
            }
            return result;
        } finally { CloseHandle(h); FreeConsole(); }
    }
}
'@
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
[ConsoleReader]::Read([uint32]$ProcessId, $Lines)
