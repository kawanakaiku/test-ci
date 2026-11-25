/* wcinput の試験の道具: Windows (GitHub のランナー) と Wine で同じ形で動かし、振る舞いを比べる
 *
 *   winprobe tab <dir>         新しいコンソールの cmd に字と Tab を入れ、補完の後の行を読む
 *   winprobe vtexit <dir>      子が VT の状態を変えたまま終わった後の、コンソールの状態を読む
 *   winprobe vtchild <log> <processed 0/1>   (vtexit が cmd から起こす子)
 *   winprobe keys <log>        入力の記録を <log> に書く (Ctrl+C を 2 回で終わる、30 秒で終わる)
 *   winprobe sel <dir>         新しいコンソールに字を書き、SendInput のマウスで選んでコピーし、クリップボードを読む
 *                              (PC の Xvfb の中だけで使う。端末の入力は adb shell input で)
 *   winprobe devsel <dir>      端末用: sel と同じ字を新しいコンソールの cmd に出し、窓の位置・セルの大きさ・
 *                              矢印の位置・入力のモード・画面の字を 300ms ごとに <dir>\state.txt に書く
 *                              (コンソールの cmd が終わるか 30 分で終わる。操作は PC から adb shell input で)
 *   winprobe ime <dir> <node>  新しいコンソールの窓へ WM_IME_CHAR (IME の確定の字) を送り、cmd の行と
 *                              raw mode の Node (<dir>\keys.js) が受けた字を数える
 *
 * 作り方: x86_64-w64-mingw32-clang -O1 -municode -o winprobe.exe winprobe.c
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static HANDLE in_h, out_h;

static void put_key( WORD vk, WCHAR ch, DWORD state, BOOL down )
{
    INPUT_RECORD r;
    DWORD n;
    memset( &r, 0, sizeof(r) );
    r.EventType = KEY_EVENT;
    r.Event.KeyEvent.bKeyDown = down;
    r.Event.KeyEvent.wRepeatCount = 1;
    r.Event.KeyEvent.wVirtualKeyCode = vk;
    r.Event.KeyEvent.wVirtualScanCode = MapVirtualKeyW( vk, 0 );
    r.Event.KeyEvent.uChar.UnicodeChar = ch;
    r.Event.KeyEvent.dwControlKeyState = state;
    WriteConsoleInputW( in_h, &r, 1, &n );
}

static void tap( WORD vk, WCHAR ch, DWORD state )
{
    put_key( vk, ch, state, TRUE );
    put_key( vk, ch, state, FALSE );
}

static void type( const WCHAR *s )
{
    for (; *s; s++)
    {
        SHORT k = VkKeyScanW( *s );
        DWORD state = (HIBYTE(k) & 1) ? SHIFT_PRESSED : 0;
        tap( k == -1 ? 0 : LOBYTE(k), *s, state );
    }
}

static void enter(void) { tap( VK_RETURN, '\r', 0 ); }

static HANDLE open_out(void)
{
    return CreateFileW( L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL );
}

/* the text of a row, without the trailing spaces */
static void read_row( HANDLE out, int y, WCHAR *buf, int width )
{
    COORD c = { 0, (SHORT)y };
    DWORD n = 0;
    ReadConsoleOutputCharacterW( out, buf, width, c, &n );
    buf[n] = 0;
    while (n && buf[n - 1] == ' ') buf[--n] = 0;
}

static void print_w( const char *pre, const WCHAR *s )
{
    char u[4096];
    WideCharToMultiByte( CP_UTF8, 0, s, -1, u, sizeof(u), NULL, NULL );
    printf( "%s%s\n", pre, u );
    fflush( stdout );
}

static void cursor_line( WCHAR *buf )
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE out = open_out();
    GetConsoleScreenBufferInfo( out, &info );
    read_row( out, info.dwCursorPosition.Y, buf, info.dwSize.X );
    CloseHandle( out );
}

static void dump_screen( const char *title )
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    CONSOLE_CURSOR_INFO ci;
    DWORD omode = 0, imode = 0;
    WCHAR buf[1024];
    HANDLE out = open_out(), in = CreateFileW( L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    int y;

    GetConsoleScreenBufferInfo( out, &info );
    GetConsoleCursorInfo( out, &ci );
    GetConsoleMode( out, &omode );
    GetConsoleMode( in, &imode );
    printf( "== %s: size %dx%d window %d..%d cursor %d,%d visible %d size %lu outmode %#lx inmode %#lx\n", title,
            info.dwSize.X, info.dwSize.Y, info.srWindow.Top, info.srWindow.Bottom,
            info.dwCursorPosition.X, info.dwCursorPosition.Y, ci.bVisible, ci.dwSize, omode, imode );
    for (y = info.srWindow.Top; y <= info.srWindow.Bottom; y++)
    {
        char pre[16];
        read_row( out, y, buf, info.dwSize.X < 1000 ? info.dwSize.X : 1000 );
        if (!buf[0]) continue;
        sprintf( pre, "%3d| ", y );
        print_w( pre, buf );
    }
    CloseHandle( out );
    CloseHandle( in );
}

static DWORD start_cmd( const WCHAR *dir, const WCHAR *args )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR cmdline[512];

    swprintf( cmdline, ARRAYSIZE(cmdline), L"cmd.exe %s", args );
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir, &si, &pi ))
    {
        printf( "CreateProcess failed %lu\n", GetLastError() );
        exit( 1 );
    }
    Sleep( 2000 );
    FreeConsole();
    if (!AttachConsole( pi.dwProcessId ))
    {
        printf( "AttachConsole failed %lu\n", GetLastError() );
        exit( 1 );
    }
    in_h = CreateFileW( L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    out_h = open_out();
    return pi.dwProcessId;
}

static void make_file( const WCHAR *path, const char *text )
{
    HANDLE f = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    DWORD n;
    WriteFile( f, text, strlen(text), &n, NULL );
    CloseHandle( f );
}

static void tree( const WCHAR *dir )
{
    WCHAR p[MAX_PATH];
    static const WCHAR *dirs[] = { L"Download", L"Download\\a", L"Download\\a\\b", L"Download\\a\\c",
                                   L"Documents", L"sub dir", L"Program Files", L"w" };
    int i;
    CreateDirectoryW( dir, NULL );
    for (i = 0; i < ARRAYSIZE(dirs); i++)
    {
        swprintf( p, MAX_PATH, L"%s\\%s", dir, dirs[i] );
        CreateDirectoryW( p, NULL );
    }
    swprintf( p, MAX_PATH, L"%s\\Download\\a\\b\\1", dir ); make_file( p, "1\r\n" );
    swprintf( p, MAX_PATH, L"%s\\Download\\a\\b\\2", dir ); make_file( p, "2\r\n" );
    swprintf( p, MAX_PATH, L"%s\\sub dir\\x.txt", dir ); make_file( p, "x\r\n" );
}

static int do_tab( const WCHAR *dir )
{
    static const WCHAR *cases[] = {
        L"dir ..\\Do", L"cd ..\\Dow", L"dir ../Dow", L"dir ../Download/a/", L"dir ..\\Download\\a\\",
        L"dir /w ../Dow", L"dir C:/Win", L"dir C:/Windows/Sys", L"dir /Windows/Sys", L"dir \\Windows\\Sys",
        L"dir ..\\sub", L"dir \"..\\sub d", L"dir ..\\Prog", L"C:\\Program Fi", L"copy ../Download/a/b/",
        L"dir ..\\Download\\a\\b/", L"dir ..\\Download/a\\b\\", L"type ../Download/a/b/1", L"dir ..\\Download/",
    };
    WCHAR w[MAX_PATH], line[1024];
    int i;

    tree( dir );
    swprintf( w, MAX_PATH, L"%s\\w", dir );
    start_cmd( w, L"/k prompt $G" );
    for (i = 0; i < ARRAYSIZE(cases); i++)
    {
        type( cases[i] );
        tap( VK_TAB, '\t', 0 );
        Sleep( 700 );
        cursor_line( line );
        print_w( "typed: ", cases[i] );
        print_w( "  tab: ", line );
        tap( VK_TAB, '\t', 0 );
        Sleep( 500 );
        cursor_line( line );
        print_w( " tab2: ", line );
        tap( VK_ESCAPE, 0x1b, 0 );
        Sleep( 300 );
    }
    type( L"exit" );
    enter();
    return 0;
}

static void child_log( const WCHAR *log, const char *fmt, ... )
{
    va_list args;
    char buf[512];
    FILE *f = _wfopen( log, L"a" );
    va_start( args, fmt );
    vsnprintf( buf, sizeof(buf), fmt, args );
    va_end( args );
    if (f) { fputs( buf, f ); fclose( f ); }
}

static const WCHAR *g_log;

static BOOL WINAPI ctrl_handler( DWORD type )
{
    child_log( g_log, "HANDLER %lu\n", type );
    return TRUE;
}

static int do_vtchild( const WCHAR *log, int processed )
{
    HANDLE in = GetStdHandle( STD_INPUT_HANDLE ), out = GetStdHandle( STD_OUTPUT_HANDLE );
    DWORD omode, imode, n, start = GetTickCount();
    int ctrlc = 0;
    const char *seq = "\x1b[?1049hALTMARK\r\n\x1b[?25l\x1b[5 q\x1b[3;10r\x1b[?1h\x1b[?2004h\x1b[5;1HALT5\r\n";

    g_log = log;
    SetConsoleCtrlHandler( ctrl_handler, TRUE );
    GetConsoleMode( out, &omode );
    GetConsoleMode( in, &imode );
    child_log( log, "START outmode %#lx inmode %#lx\n", omode, imode );
    SetConsoleMode( out, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING );
    WriteConsoleA( out, seq, strlen(seq), &n, NULL );
    SetConsoleMode( in, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | (processed ? ENABLE_PROCESSED_INPUT : 0) );
    GetConsoleMode( in, &imode );
    child_log( log, "SET inmode %#lx\n", imode );
    while (GetTickCount() - start < 8000 && ctrlc < 2)
    {
        INPUT_RECORD r;
        if (WaitForSingleObject( in, 200 )) continue;
        if (!ReadConsoleInputW( in, &r, 1, &n ) || !n) break;
        if (r.EventType == KEY_EVENT)
        {
            child_log( log, "KEY down %d vk %#x scan %#x ch %#x state %#lx repeat %d\n", r.Event.KeyEvent.bKeyDown,
                       r.Event.KeyEvent.wVirtualKeyCode, r.Event.KeyEvent.wVirtualScanCode,
                       r.Event.KeyEvent.uChar.UnicodeChar, r.Event.KeyEvent.dwControlKeyState,
                       r.Event.KeyEvent.wRepeatCount );
            if (r.Event.KeyEvent.bKeyDown && r.Event.KeyEvent.uChar.UnicodeChar == 3) ctrlc++;
        }
        else child_log( log, "EVENT %d\n", r.EventType );
    }
    child_log( log, "EXIT ctrlc %d\n", ctrlc );
    return 0;
}

static void print_file( const WCHAR *path )
{
    char buf[256];
    FILE *f = _wfopen( path, L"r" );
    if (!f) { printf( "(no log)\n" ); return; }
    while (fgets( buf, sizeof(buf), f )) printf( "  log: %s", buf );
    fclose( f );
}

static int wait_exit( const WCHAR *log )
{
    int i;
    for (i = 0; i < 100; i++)
    {
        char buf[256];
        FILE *f = _wfopen( log, L"r" );
        BOOL done = FALSE;
        if (f)
        {
            while (fgets( buf, sizeof(buf), f )) if (!strncmp( buf, "EXIT", 4 )) done = TRUE;
            fclose( f );
        }
        if (done) return 1;
        Sleep( 200 );
    }
    return 0;
}

static void ctrl_c(void)
{
    put_key( VK_CONTROL, 0, LEFT_CTRL_PRESSED, TRUE );
    put_key( 'C', 3, LEFT_CTRL_PRESSED, TRUE );
    put_key( 'C', 3, LEFT_CTRL_PRESSED, FALSE );
    put_key( VK_CONTROL, 0, 0, FALSE );
}

static int do_vtexit( const WCHAR *dir )
{
    WCHAR self[MAX_PATH], log[MAX_PATH], line[600];
    int processed;

    GetModuleFileNameW( NULL, self, MAX_PATH );
    CreateDirectoryW( dir, NULL );
    start_cmd( dir, L"/k prompt $G" );
    /* the processed child's Ctrl+C goes to every process of the console, also to this one */
    SetConsoleCtrlHandler( NULL, TRUE );
    type( L"echo MAINMARK" );
    enter();
    Sleep( 800 );
    for (processed = 0; processed < 2; processed++)
    {
        swprintf( log, MAX_PATH, L"%s\\vtchild%d.log", dir, processed );
        DeleteFileW( log );
        swprintf( line, ARRAYSIZE(line), L"\"%s\" vtchild \"%s\" %d", self, log, processed );
        type( line );
        enter();
        Sleep( 1500 );
        dump_screen( processed ? "child running (processed)" : "child running (raw)" );
        ctrl_c();
        Sleep( 400 );
        ctrl_c();
        printf( "child exited: %d\n", wait_exit( log ) );
        print_file( log );
        Sleep( 1000 );
        dump_screen( "after exit" );
        type( L"for /l %i in (1,1,14) do @echo L%i" );
        enter();
        Sleep( 1500 );
        dump_screen( "after echo" );
    }
    type( L"exit" );
    enter();
    return 0;
}

/* the input records, for comparing the keys (Ctrl+C, IME) between Windows and Wine */
static int do_keys( const WCHAR *log )
{
    return do_vtchild( log, 0 );
}

/* ---- sel: selection and copy with the mouse (quick edit) ---- */
static HWND sel_win;
static int sel_cw, sel_ch;
static POINT sel_origin;

static void sel_mouse( int col, int row, DWORD flags )
{
    INPUT in = { INPUT_MOUSE };
    int sw = GetSystemMetrics( SM_CXSCREEN ), sh = GetSystemMetrics( SM_CYSCREEN );
    int x = sel_origin.x + col * sel_cw + sel_cw / 2, y = sel_origin.y + row * sel_ch + sel_ch / 2;
    in.mi.dx = (x * 65535 + sw / 2) / (sw - 1);
    in.mi.dy = (y * 65535 + sh / 2) / (sh - 1);
    in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | flags;
    SendInput( 1, &in, sizeof(in) );
    Sleep( 60 );
}

static void sel_key( WORD vk, BOOL down )
{
    INPUT in = { INPUT_KEYBOARD };
    in.ki.wVk = vk;
    in.ki.wScan = MapVirtualKeyW( vk, 0 );
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput( 1, &in, sizeof(in) );
    Sleep( 40 );
}

static void sel_drag( int c0, int r0, int c1, int r1, BOOL alt )
{
    int i;
    if (alt) sel_key( VK_MENU, TRUE );
    sel_mouse( c0, r0, 0 );
    sel_mouse( c0, r0, MOUSEEVENTF_LEFTDOWN );
    for (i = 1; i <= 4; i++) sel_mouse( c0 + (c1 - c0) * i / 4, r0 + (r1 - r0) * i / 4, 0 );
    sel_mouse( c1, r1, MOUSEEVENTF_LEFTUP );
    if (alt) sel_key( VK_MENU, FALSE );
    Sleep( 200 );
}

static void sel_clear_clipboard(void)
{
    if (OpenClipboard( NULL )) { EmptyClipboard(); CloseClipboard(); }
}

static void sel_print_clipboard( const char *name, const WCHAR *expect )
{
    WCHAR text[2048] = L"(none)";
    HANDLE h;
    Sleep( 300 );
    if (OpenClipboard( NULL ))
    {
        if ((h = GetClipboardData( CF_UNICODETEXT )))
        {
            WCHAR *p = GlobalLock( h );
            if (p) { lstrcpynW( text, p, ARRAYSIZE(text) ); GlobalUnlock( h ); }
        }
        CloseClipboard();
    }
    {
        WCHAR shown[4096], *d = shown;
        const WCHAR *c;
        for (c = text; *c && d < shown + ARRAYSIZE(shown) - 4; c++)
        {
            if (*c == '\r') { *d++ = '\\'; *d++ = 'r'; }
            else if (*c == '\n') { *d++ = '\\'; *d++ = 'n'; }
            else *d++ = *c;
        }
        *d = 0;
        printf( "%s: %s (%d chars) ", name, expect ? (wcscmp( text, expect ) ? "NG" : "OK") : "--", lstrlenW( text ) );
        print_w( "[", shown );
    }
}

static int do_sel( const WCHAR *dir )
{
    static const WCHAR jp[] = L"日本語のテキストです";  /* 日本語のテキストです */
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CONSOLE_SCREEN_BUFFER_INFO info;
    WCHAR cmdline[64] = L"cmd.exe /k", url[256], url2[256], expect[512], row[256];
    RECT rc;
    DWORD n, mode;
    int i, width, top;

    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir, &si, &pi ))
    {
        printf( "CreateProcess failed %lu\n", GetLastError() );
        return 1;
    }
    Sleep( 2500 );
    FreeConsole();
    if (!AttachConsole( pi.dwProcessId )) { printf( "AttachConsole failed %lu\n", GetLastError() ); return 1; }
    in_h = CreateFileW( L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    out_h = open_out();
    GetConsoleMode( in_h, &mode );
    printf( "input mode %#lx (quick edit %s)\n", mode, mode & ENABLE_QUICK_EDIT_MODE ? "on" : "off" );
    GetConsoleScreenBufferInfo( out_h, &info );
    width = info.dwSize.X;
    /* the test text, written by cmd (type) from row 0: hello (0), a URL wrapped by the console
     * (1..3), a URL cut by the program at the width (4, 5), Japanese (6) */
    {
        char text[2048], *p = text;
        p += sprintf( p, "hello world   \r\n" );
        wcscpy( url, L"https://example.com/" );
        for (i = lstrlenW( url ); i < 200; i++) url[i] = 'a' + i % 26;
        url[200] = 0;
        p += sprintf( p, "%ls\r\n", url );
        wcscpy( url2, L"https://example.org/ink?" );
        for (i = lstrlenW( url2 ); i < 130; i++) url2[i] = 'A' + i % 26;
        url2[130] = 0;
        p += sprintf( p, "%.*ls\r\n%ls\r\n", width, url2, url2 + width );
        p += WideCharToMultiByte( CP_UTF8, 0, jp, -1, p, 256, NULL, NULL ) - 1;
        p += sprintf( p, "\r\n" );
        swprintf( row, ARRAYSIZE(row), L"%s\\sel.txt", dir );
        make_file( row, text );
    }
    type( L"chcp 65001 >nul & cls & type sel.txt" );
    enter();
    Sleep( 1500 );
    GetConsoleScreenBufferInfo( out_h, &info );
    top = info.srWindow.Top;
    for (i = top; i < top + 9; i++)
    {
        char pre[16];
        read_row( out_h, i, row, width );
        sprintf( pre, "%3d| ", i - top );
        print_w( pre, row );
    }

    sel_win = GetConsoleWindow();
    GetClientRect( sel_win, &rc );
    sel_cw = rc.right / (info.srWindow.Right - info.srWindow.Left + 1);
    sel_ch = rc.bottom / (info.srWindow.Bottom - info.srWindow.Top + 1);
    sel_origin.x = sel_origin.y = 0;
    ClientToScreen( sel_win, &sel_origin );
    SetForegroundWindow( sel_win );
    printf( "window %p client %ldx%ld cell %dx%d origin %ld,%ld\n", sel_win, rc.right, rc.bottom, sel_cw, sel_ch,
            sel_origin.x, sel_origin.y );
    Sleep( 300 );

    sel_clear_clipboard();
    sel_drag( 0, 0, 10, 0, FALSE );
    sel_mouse( 5, 0, MOUSEEVENTF_RIGHTDOWN ); sel_mouse( 5, 0, MOUSEEVENTF_RIGHTUP );
    sel_print_clipboard( "drag+right click", L"hello world" );

    sel_clear_clipboard();
    sel_drag( 0, 1, (200 - 1) % width, 1 + 199 / width, FALSE );
    sel_key( VK_RETURN, TRUE ); sel_key( VK_RETURN, FALSE );
    sel_print_clipboard( "wrapped URL drag+Enter", url );

    sel_clear_clipboard();
    sel_mouse( 30, 2, MOUSEEVENTF_LEFTDOWN ); sel_mouse( 30, 2, MOUSEEVENTF_LEFTUP );
    sel_mouse( 30, 2, MOUSEEVENTF_LEFTDOWN ); sel_mouse( 30, 2, MOUSEEVENTF_LEFTUP );
    Sleep( 200 );
    sel_key( VK_CONTROL, TRUE ); sel_key( 'C', TRUE ); sel_key( 'C', FALSE ); sel_key( VK_CONTROL, FALSE );
    sel_print_clipboard( "wrapped URL double click+Ctrl+C", url );

    sel_clear_clipboard();
    sel_mouse( 10, 5, MOUSEEVENTF_LEFTDOWN ); sel_mouse( 10, 5, MOUSEEVENTF_LEFTUP );
    sel_mouse( 10, 5, MOUSEEVENTF_LEFTDOWN ); sel_mouse( 10, 5, MOUSEEVENTF_LEFTUP );
    Sleep( 200 );
    sel_key( VK_CONTROL, TRUE ); sel_key( 'C', TRUE ); sel_key( 'C', FALSE ); sel_key( VK_CONTROL, FALSE );
    sel_print_clipboard( "cut URL (ink) double click+Ctrl+C", url2 );

    sel_clear_clipboard();
    sel_drag( 0, 6, 19, 6, FALSE );
    sel_mouse( 5, 6, MOUSEEVENTF_RIGHTDOWN ); sel_mouse( 5, 7, MOUSEEVENTF_RIGHTUP );
    sel_print_clipboard( "japanese drag+right click", jp );

    sel_clear_clipboard();
    sel_drag( 6, 0, 4, 1, FALSE );
    sel_key( VK_RETURN, TRUE ); sel_key( VK_RETURN, FALSE );
    swprintf( expect, ARRAYSIZE(expect), L"world\r\nhttps" );
    sel_print_clipboard( "two lines drag+Enter", expect );

    /* Alt + drag: PC's X11 driver drops the Alt of SendInput with the pointer's X events, so the
     * mouse messages are posted to the window, with Alt held by SendInput */
    sel_clear_clipboard();
    sel_key( VK_MENU, TRUE );
    PostMessageW( sel_win, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM( 0 * sel_cw + 2, 0 * sel_ch + 2 ) );
    PostMessageW( sel_win, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM( 4 * sel_cw + 2, 1 * sel_ch + 2 ) );
    PostMessageW( sel_win, WM_LBUTTONUP, 0, MAKELPARAM( 4 * sel_cw + 2, 1 * sel_ch + 2 ) );
    Sleep( 200 );
    sel_key( VK_MENU, FALSE );
    Sleep( 200 );
    if (0) sel_drag( 0, 0, 4, 1, TRUE );
    sel_key( VK_RETURN, TRUE ); sel_key( VK_RETURN, FALSE );
    sel_print_clipboard( "block (Alt) drag+Enter", L"hello\r\nhttps" );

    sel_clear_clipboard();
    sel_drag( 0, 0, 10, 0, FALSE );
    sel_key( VK_ESCAPE, TRUE ); sel_key( VK_ESCAPE, FALSE );
    sel_mouse( 5, 0, MOUSEEVENTF_RIGHTDOWN ); sel_mouse( 5, 0, MOUSEEVENTF_RIGHTUP );
    sel_print_clipboard( "drag+Esc+right click (nothing copied)", NULL );

    /* a tap leaves no selection: the right click pastes into the prompt */
    {
        HANDLE mem = GlobalAlloc( GMEM_MOVEABLE, 32 );
        wcscpy( GlobalLock( mem ), L"echo pasted" );
        GlobalUnlock( mem );
        if (OpenClipboard( NULL )) { EmptyClipboard(); SetClipboardData( CF_UNICODETEXT, mem ); CloseClipboard(); }
    }
    sel_mouse( 3, 1, MOUSEEVENTF_LEFTDOWN ); sel_mouse( 3, 1, MOUSEEVENTF_LEFTUP );
    Sleep( 300 );
    sel_mouse( 3, 1, MOUSEEVENTF_RIGHTDOWN ); sel_mouse( 3, 1, MOUSEEVENTF_RIGHTUP );
    Sleep( 500 );
    cursor_line( row );
    printf( "tap+right click pastes: %s ", wcsstr( row, L"echo pasted" ) ? "OK" : "NG" );
    print_w( "[", row );
    tap( VK_ESCAPE, 0x1b, 0 );
    type( L"exit" );
    enter();
    return 0;
}

/* ---- devsel: the state of a console for the device tests (the input comes from adb shell input) ---- */
/* written into the open file: looking up a path each time makes Android log an SELinux denial
 * (an ioctl on the folder) and floods logcat */
static void devsel_state( HANDLE file, HWND win )
{
    static char buf[65536];
    CONSOLE_SCREEN_BUFFER_INFO info;
    WCHAR row[512];
    POINT cur, org = { 0, 0 };
    RECT rc;
    DWORD mode = 0, omode = 0;
    char *p = buf, u[2048];
    HANDLE out = open_out(), in = CreateFileW( L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    int i, cols = 1, rows = 1;

    GetConsoleScreenBufferInfo( out, &info );
    GetConsoleMode( in, &mode );
    GetConsoleMode( out, &omode );
    GetCursorPos( &cur );
    GetClientRect( win, &rc );
    ClientToScreen( win, &org );
    cols = info.srWindow.Right - info.srWindow.Left + 1;
    rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    p += sprintf( p, "screen %d %d\r\ncursor %ld %ld\r\nclient %ld %ld %ld %ld cell %ld %ld\r\n",
                  GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ), cur.x, cur.y, org.x, org.y,
                  rc.right, rc.bottom, rc.right / cols, rc.bottom / rows );
    p += sprintf( p, "mode %#lx out %#lx foreground %d\r\nconsole cursor %d %d top %d size %d %d\r\n", mode, omode,
                  GetForegroundWindow() == win, info.dwCursorPosition.X, info.dwCursorPosition.Y - info.srWindow.Top,
                  info.srWindow.Top, info.dwSize.X, info.dwSize.Y );
    for (i = 0; i < rows && p < buf + sizeof(buf) - 4200; i++)
    {
        read_row( out, info.srWindow.Top + i, row, min( info.dwSize.X, 500 ) );
        WideCharToMultiByte( CP_UTF8, 0, row, -1, u, sizeof(u), NULL, NULL );
        p += sprintf( p, "%03d|%s\r\n", i, u );
    }
    CloseHandle( out );
    CloseHandle( in );
    p += sprintf( p, "END\r\n" );
    {
        DWORD n;
        SetFilePointer( file, 0, NULL, FILE_BEGIN );
        WriteFile( file, buf, p - buf, &n, NULL );
        SetEndOfFile( file );
    }
}

static int do_devsel( const WCHAR *dir )
{
    static const WCHAR jp[] = L"日本語のテキストです";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CONSOLE_SCREEN_BUFFER_INFO info;
    WCHAR cmdline[64] = L"cmd.exe /k", url[256], url2[256], path[MAX_PATH];
    HANDLE file;
    HWND win;
    int i, width;

    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir, &si, &pi )) return 1;
    Sleep( 3000 );
    FreeConsole();
    if (!AttachConsole( pi.dwProcessId )) return 1;
    in_h = CreateFileW( L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    out_h = open_out();
    GetConsoleScreenBufferInfo( out_h, &info );
    width = info.dwSize.X;
    {
        char text[2048], *p = text;
        p += sprintf( p, "hello world   \r\n" );
        wcscpy( url, L"https://example.com/" );
        for (i = lstrlenW( url ); i < 200; i++) url[i] = 'a' + i % 26;
        url[200] = 0;
        p += sprintf( p, "%ls\r\n", url );
        wcscpy( url2, L"https://example.org/ink?" );
        for (i = lstrlenW( url2 ); i < 130; i++) url2[i] = 'A' + i % 26;
        url2[130] = 0;
        p += sprintf( p, "%.*ls\r\n%ls\r\n", width, url2, url2 + width );
        p += WideCharToMultiByte( CP_UTF8, 0, jp, -1, p, 256, NULL, NULL ) - 1;
        p += sprintf( p, "\r\n" );
        swprintf( path, ARRAYSIZE(path), L"%s\\sel.txt", dir );
        make_file( path, text );
    }
    type( L"chcp 65001 >nul & cls & type sel.txt" );
    enter();
    win = GetConsoleWindow();
    swprintf( path, ARRAYSIZE(path), L"%s\\state.txt", dir );
    file = CreateFileW( path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, 0, NULL );
    /* ends with the console's cmd (type exit), or after 30 minutes */
    for (i = 0; i < 30 * 60 * 1000 / 300; i++)
    {
        if (WaitForSingleObject( pi.hProcess, 300 ) == WAIT_OBJECT_0) break;
        devsel_state( file, win );
    }
    CloseHandle( file );
    return 0;
}

/* ---- ime: the IME's committed chars (WM_IME_CHAR to the console window), in cmd and in raw mode Node ---- */
static void ime_chars( HWND win, const WCHAR *s )
{
    for (; *s; s++) PostMessageW( win, WM_IME_CHAR, *s, 1 );
}

static int do_ime( const WCHAR *dir, const WCHAR *node )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR cmdline[64] = L"cmd.exe /k", row[512], line[512], path[MAX_PATH];
    char buf[8192];
    HWND win;
    HANDLE f;
    DWORD n = 0;

    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir, &si, &pi )) return 1;
    Sleep( 2500 );
    FreeConsole();
    if (!AttachConsole( pi.dwProcessId )) return 1;
    in_h = CreateFileW( L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    out_h = open_out();
    win = GetConsoleWindow();
    ime_chars( win, L"日本語" );
    Sleep( 1500 );
    cursor_line( row );
    /* ReadConsoleOutputCharacter of Wine gives a wide char once for each of its two cells */
    {
        int i, kanji = 0;
        for (i = 0; row[i]; i++) if (row[i] == L'日') kanji++;
        printf( "cmd: 日本語 (日 %d セル、期待 1 字 = 2 セル) %s ", kanji, kanji == 2 || (kanji == 1 && wcsstr( row, L"日本" )) ? "OK" : "NG" );
    }
    print_w( "[", row );
    tap( VK_ESCAPE, 0x1b, 0 );
    swprintf( line, ARRAYSIZE(line), L"\"%s\" keys.js keys.log", node );
    type( line );
    enter();
    Sleep( 6000 );
    ime_chars( win, L"日本語" );
    Sleep( 1500 );
    ime_chars( win, L"あ" );
    Sleep( 1500 );
    /* Ctrl+C twice ends keys.js (raw mode: it is the char 3) */
    tap( 'C', 3, LEFT_CTRL_PRESSED );
    Sleep( 500 );
    tap( 'C', 3, LEFT_CTRL_PRESSED );
    Sleep( 3000 );
    swprintf( path, ARRAYSIZE(path), L"%s\\keys.log", dir );
    f = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if (f != INVALID_HANDLE_VALUE) { ReadFile( f, buf, sizeof(buf) - 1, &n, NULL ); CloseHandle( f ); }
    buf[n] = 0;
    {
        WCHAR w[8192];
        int i, kanji = 0, a = 0, len = MultiByteToWideChar( CP_UTF8, 0, buf, n, w, ARRAYSIZE(w) - 1 );
        for (i = 0; i < len; i++) { if (w[i] == L'日') kanji++; if (w[i] == L'あ') a++; }
        printf( "node: 日 %d 回、あ %d 回 (期待 1 と 1) %s, log %lu バイト\n", kanji, a, kanji == 1 && a == 1 ? "OK" : "NG", n );
        printf( "%.600s\n", buf );
    }
    type( L"exit" );
    enter();
    return 0;
}

int wmain( int argc, WCHAR **argv )
{
    if (argc >= 3 && !wcscmp( argv[1], L"tab" )) return do_tab( argv[2] );
    if (argc >= 3 && !wcscmp( argv[1], L"vtexit" )) return do_vtexit( argv[2] );
    if (argc >= 4 && !wcscmp( argv[1], L"vtchild" )) return do_vtchild( argv[2], _wtoi( argv[3] ) );
    if (argc >= 3 && !wcscmp( argv[1], L"keys" )) return do_keys( argv[2] );
    if (argc >= 3 && !wcscmp( argv[1], L"sel" )) return do_sel( argv[2] );
    if (argc >= 3 && !wcscmp( argv[1], L"devsel" )) return do_devsel( argv[2] );
    if (argc >= 4 && !wcscmp( argv[1], L"ime" )) return do_ime( argv[2], argv[3] );
    printf( "usage: winprobe tab|vtexit <dir> (argc %d, argv[1] %ls)\n", argc, argc > 1 ? argv[1] : L"" );
    return 1;
}
