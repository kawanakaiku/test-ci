// WindowState を Maximized にした窓を出したときの、窓の通知と WindowState・GetWindowPlacement の順番を記録する。
// MikuMikuMoving (Game.Run) と同じく、Resize で「前の WindowState」と比べ、Minimized を経たかを数える。
//   csc /define:DX /r:System\x64\DevExpress.XtraBars.v14.2.dll ... で、MMM と同じ DevExpress の RibbonForm を継ぐ版になる。
//   maxprobe.exe [Maximized|Normal] [秒]   記録は exe と同じフォルダーの maxprobe-<版>.txt に足す
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

#if DX
class ProbeForm : DevExpress.XtraBars.Ribbon.RibbonForm
#else
class ProbeForm : Form
#endif
{
    [StructLayout(LayoutKind.Sequential)]
    struct WINDOWPLACEMENT { public int length, flags, showCmd, minX, minY, maxX, maxY, left, top, right, bottom; }
    [DllImport("user32.dll")] static extern bool GetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT wp);
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr hwnd, int index);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsZoomed(IntPtr hwnd);

    public static StreamWriter Log;
    public static Stopwatch Clock = Stopwatch.StartNew();
    public FormWindowState OldState;
    public int MinimizedSeen, FromMinimized;

    public static void Write(string s)
    {
        Log.WriteLine(Clock.ElapsedMilliseconds.ToString("D5") + " " + s);
        Log.Flush();
    }

    string State()
    {
        string s = "ws=" + WindowState;
        if (!IsHandleCreated) return s + " (no handle)";
        WINDOWPLACEMENT wp = new WINDOWPLACEMENT();
        wp.length = Marshal.SizeOf(wp);
        GetWindowPlacement(Handle, ref wp);
        return s + " show=" + wp.showCmd + " style=" + GetWindowLong(Handle, -16).ToString("x8")
            + " iconic=" + (IsIconic(Handle) ? 1 : 0) + " zoomed=" + (IsZoomed(Handle) ? 1 : 0) + " bounds=" + Bounds;
    }

    static string MsgName(int msg)
    {
        switch (msg)
        {
        case 0x0001: return "WM_CREATE";
        case 0x0003: return "WM_MOVE";
        case 0x0005: return "WM_SIZE";
        case 0x0006: return "WM_ACTIVATE";
        case 0x0018: return "WM_SHOWWINDOW";
        case 0x001C: return "WM_ACTIVATEAPP";
        case 0x0024: return "WM_GETMINMAXINFO";
        case 0x0046: return "WM_WINDOWPOSCHANGING";
        case 0x0047: return "WM_WINDOWPOSCHANGED";
        case 0x0081: return "WM_NCCREATE";
        case 0x0083: return "WM_NCCALCSIZE";
        case 0x0086: return "WM_NCACTIVATE";
        case 0x0112: return "WM_SYSCOMMAND";
        case 0x0007: return "WM_SETFOCUS";
        case 0x0014: return "WM_ERASEBKGND";
        case 0x000F: return "WM_PAINT";
        case 0x0085: return "WM_NCPAINT";
        }
        return null;
    }

    protected override void WndProc(ref Message m)
    {
        string name = MsgName(m.Msg);
        if (name != null)
        {
            string extra = "";
            if (m.Msg == 0x0046 || m.Msg == 0x0047)
            {
                // WINDOWPOS: hwnd, hwndInsertAfter, x, y, cx, cy, flags
                int off = IntPtr.Size * 2;
                extra = " pos=" + Marshal.ReadInt32(m.LParam, off) + "," + Marshal.ReadInt32(m.LParam, off + 4)
                    + " size=" + Marshal.ReadInt32(m.LParam, off + 8) + "x" + Marshal.ReadInt32(m.LParam, off + 12)
                    + " flags=" + Marshal.ReadInt32(m.LParam, off + 16).ToString("x");
            }
            else if (m.Msg == 0x0005)
                extra = " size=" + ((long)m.LParam & 0xffff) + "x" + (((long)m.LParam >> 16) & 0xffff);
            Write("> " + name + " w=" + m.WParam.ToString("x") + extra + " " + State());
            base.WndProc(ref m);
            Write("< " + name + " " + State());
        }
        else base.WndProc(ref m);
    }

    protected override void OnHandleCreated(EventArgs e) { Write("OnHandleCreated " + State()); base.OnHandleCreated(e); }
    protected override void OnLoad(EventArgs e) { Write("OnLoad " + State()); base.OnLoad(e); }
    protected override void OnShown(EventArgs e) { Write("OnShown " + State()); base.OnShown(e); }
    protected override void OnSizeChanged(EventArgs e) { Write("OnSizeChanged " + State()); base.OnSizeChanged(e); }

    public void OnResizeHandler(object sender, EventArgs e)
    {
        // MikuMikuMoving の Game.<Run>b__18_2 と同じ比べ方
        FormWindowState now = WindowState;
        string note = "";
        if (now != OldState)
        {
            if (now == FormWindowState.Minimized) MinimizedSeen++;
            else if (OldState == FormWindowState.Minimized) { FromMinimized++; note = " ← MMM はここで NRE"; }
            OldState = now;
        }
        Write("Resize " + State() + note);
    }

    [STAThread]
    static int Main(string[] args)
    {
        string state = args.Length > 0 ? args[0] : "Maximized";
        int seconds = args.Length > 1 ? int.Parse(args[1]) : 5;
        string dir = Path.GetDirectoryName(Application.ExecutablePath);
#if DX
        string kind = "dx";
#else
        string kind = "form";
#endif
        Log = new StreamWriter(Path.Combine(dir, "maxprobe-" + kind + ".txt"), true);
        Write("=== maxprobe " + kind + " " + state + " " + (IntPtr.Size * 8) + "bit " + Environment.OSVersion);
        ProbeForm form = new ProbeForm();
        form.Text = "maxprobe";
        form.WindowState = (FormWindowState)Enum.Parse(typeof(FormWindowState), state);
        form.Resize += form.OnResizeHandler;
        form.OldState = form.WindowState;
        Write("Show");
        form.Show();   // SlimDX の MessagePump.Run と同じく、Show してから自前で回す
        Write("Shown");
        Stopwatch t = Stopwatch.StartNew();
        while (t.ElapsedMilliseconds < seconds * 1000 && !form.IsDisposed)
        {
            Application.DoEvents();
            System.Threading.Thread.Sleep(10);
        }
        Write("RESULT minimized_seen=" + form.MinimizedSeen + " from_minimized=" + form.FromMinimized + " final " + form.State());
        Log.Close();
        return form.FromMinimized;
    }
}
