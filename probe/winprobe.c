/* wcinput の試験の道具: Windows (GitHub のランナー) と Wine で同じ形で動かし、振る舞いを比べる
 *
 *   winprobe tab <dir>         新しいコンソールの cmd に字と Tab を入れ、補完の後の行を読む
 *   winprobe vtexit <dir>      子が VT の状態を変えたまま終わった後の、コンソールの状態を読む
 *   winprobe vtchild <log> <processed 0/1>   (vtexit が cmd から起こす子)
 *   winprobe keys <log>        入力の記録を <log> に書く (Ctrl+C を 2 回で終わる、30 秒で終わる)
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

int wmain( int argc, WCHAR **argv )
{
    if (argc >= 3 && !wcscmp( argv[1], L"tab" )) return do_tab( argv[2] );
    if (argc >= 3 && !wcscmp( argv[1], L"vtexit" )) return do_vtexit( argv[2] );
    if (argc >= 4 && !wcscmp( argv[1], L"vtchild" )) return do_vtchild( argv[2], _wtoi( argv[3] ) );
    if (argc >= 3 && !wcscmp( argv[1], L"keys" )) return do_keys( argv[2] );
    printf( "usage: winprobe tab|vtexit <dir>\n" );
    return 1;
}
