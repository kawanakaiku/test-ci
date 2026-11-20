// CreateWindowEx の途中、作る窓の WM_NCCREATE より前に、ほかの窓へ届く通知を WH_CALLWNDPROC のフックで数える。
// Wine の win32u の apply_window_pos は、owner_hint を探す WindowFromPoint で、作る窓の左上の 1 つ外の点にある窓へ
// WM_NCHITTEST を送っていた (MikuMikuMoving の NRE の元)。Windows でも送られるかを比べる。
//   csc /platform:x64 /target:winexe nchit.cs   →  nchit.exe [記録のファイル]  (既定は exe と同じ所の nchit.txt)
// 試験: 見える親 (0,0 800x600) を作り、その上に (a) 子の窓 (b) 持ち主の無いトップレベルの窓 を、左上 (358,170) で作る。
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

static class NcHit
{
    delegate IntPtr WndProc(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    delegate IntPtr HookProc(int code, IntPtr wp, IntPtr lp);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WNDCLASS { public uint style; public WndProc proc; public int cls, wnd; public IntPtr inst, icon, cursor, brush; public string menu, name; }
    [StructLayout(LayoutKind.Sequential)]
    struct CWPSTRUCT { public IntPtr lParam, wParam; public uint message; public IntPtr hwnd; }
    [StructLayout(LayoutKind.Sequential)]
    struct MSG { public IntPtr hwnd; public uint message; public IntPtr wp, lp; public uint time; public int x, y; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClassW(ref WNDCLASS wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] static extern IntPtr SetWindowsHookExW(int id, HookProc proc, IntPtr mod, uint thread);
    [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr hook);
    [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr hook, int code, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] static extern bool UpdateWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool PeekMessageW(out MSG msg, IntPtr hwnd, uint min, uint max, uint remove);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref MSG msg);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(POINT pt);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandleW(IntPtr name);
    [StructLayout(LayoutKind.Sequential)] struct POINT { public int x, y; }

    const uint WS_OVERLAPPEDWINDOW = 0x00CF0000, WS_VISIBLE = 0x10000000, WS_CHILD = 0x40000000, WS_POPUP = 0x80000000;
    const uint WM_NCCREATE = 0x0081, WM_NCHITTEST = 0x0084;

    static StringBuilder log = new StringBuilder();
    static WndProc proc = Proc;
    static HookProc hookProc = Hook;
    static IntPtr parent, creating;     // creating: 今作っている窓の名前の目印 (作る前は 0)
    static bool inCreate, sawNcCreate;
    static int before, nchitBefore;

    static IntPtr Proc(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp)
    {
        if (msg == WM_NCCREATE && inCreate && !sawNcCreate) { sawNcCreate = true; creating = hwnd; }
        if (msg == WM_NCHITTEST && hwnd == parent && inCreate) log.AppendLine("  WM_NCHITTEST to the parent (window proc), before NCCREATE=" + !sawNcCreate);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static IntPtr Hook(int code, IntPtr wp, IntPtr lp)
    {
        if (code >= 0 && inCreate && !sawNcCreate)
        {
            CWPSTRUCT c = (CWPSTRUCT)Marshal.PtrToStructure(lp, typeof(CWPSTRUCT));
            if (c.message != WM_NCCREATE)
            {
                before++;
                if (c.message == WM_NCHITTEST) nchitBefore++;
                log.AppendLine("  hook before NCCREATE: msg=0x" + c.message.ToString("x4") + (c.hwnd == parent ? " to the parent" : " hwnd=" + c.hwnd.ToString("x")));
            }
        }
        return CallNextHookEx(IntPtr.Zero, code, wp, lp);
    }

    static void Pump()
    {
        MSG m;
        for (int i = 0; i < 200 && PeekMessageW(out m, IntPtr.Zero, 0, 0, 1); i++) { TranslateMessage(ref m); DispatchMessageW(ref m); }
    }

    static void Create(string label, uint style, IntPtr owner)
    {
        before = nchitBefore = 0; sawNcCreate = false; inCreate = true;
        IntPtr hwnd = CreateWindowExW(0, "nchit", label, style, 358, 170, 300, 200, owner, IntPtr.Zero, GetModuleHandleW(IntPtr.Zero), IntPtr.Zero);
        inCreate = false;
        log.AppendLine(label + ": hwnd=" + (hwnd != IntPtr.Zero) + " messages to other windows before NCCREATE=" + before + " WM_NCHITTEST=" + nchitBefore);
        Pump();
        if (hwnd != IntPtr.Zero) DestroyWindow(hwnd);
    }

    [STAThread]
    static int Main(string[] args)
    {
        string path = args.Length > 0 ? args[0] : Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "nchit.txt");
        log.AppendLine("=== nchit " + (IntPtr.Size * 8) + "bit " + Environment.OSVersion);
        WNDCLASS wc = new WNDCLASS();
        wc.proc = proc; wc.inst = GetModuleHandleW(IntPtr.Zero); wc.name = "nchit"; wc.brush = (IntPtr)6;
        RegisterClassW(ref wc);
        parent = CreateWindowExW(0, "nchit", "nchit parent", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 800, 600, IntPtr.Zero, IntPtr.Zero, wc.inst, IntPtr.Zero);
        ShowWindow(parent, 1); UpdateWindow(parent);
        for (int i = 0; i < 20; i++) { Pump(); System.Threading.Thread.Sleep(50); }
        POINT pt; pt.x = 357; pt.y = 169;
        log.AppendLine("WindowFromPoint(357,169) is the parent: " + (WindowFromPoint(pt) == parent));
        IntPtr hook = SetWindowsHookExW(4 /* WH_CALLWNDPROC */, hookProc, IntPtr.Zero, GetCurrentThreadId());
        Create("child", WS_CHILD | WS_VISIBLE, parent);
        Create("top-level popup", WS_POPUP | WS_VISIBLE, IntPtr.Zero);
        Create("top-level overlapped", WS_OVERLAPPEDWINDOW | WS_VISIBLE, IntPtr.Zero);
        UnhookWindowsHookEx(hook);
        DestroyWindow(parent);
        File.AppendAllText(path, log.ToString());
        return 0;
    }
}
