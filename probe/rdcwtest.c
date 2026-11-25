/* フォルダーの変化の知らせ (ReadDirectoryChangesW・FindFirstChangeNotification) の試験。
 *
 * 動かし方 (引数か環境変数):
 *   rdcwtest.exe [self|watch|big] [<基の場所>]
 *   RDCW_MODE=self|watch|big  RDCW_DIR=<基の場所>  RDCW_OUT=<結果のファイル>
 *   RDCW_SECONDS=<watch の秒数>  RDCW_SUBTREE=0|1  RDCW_NDIRS=<big のフォルダーの数>
 *   RDCW_GO=<ファイル> (big: このファイルができるまで変更を待つ)
 *
 * self:  自分で作る・書く・名前を変える・属性を変える・消すを行い、届いた知らせを書く。
 *        同期・重ね合わせ・完了ポート・完了ルーチン、FindFirstChangeNotification、サブフォルダー、絞りを試す。
 * watch: 基の場所を見て、外からの変更で届いた知らせを時刻つきで書く。終わりに自分の CPU 時間を書く。
 * idle:  見ないで RDCW_SECONDS 待つ (CPU の比べ)。
 * big:   基の場所の下にフォルダーを RDCW_NDIRS 個作り (2 段)、サブフォルダーを含めて見て、外からの変更を書く。
 *
 * 結果の行は「CASE <名前>: <知らせ>|<知らせ>...」の形。Windows の結果と行ごとに比べる。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

static FILE *out;
static WCHAR base[MAX_PATH];
static DWORD t0;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
}

static void u8(const WCHAR *w, int wlen, char *buf, int n)
{
    int r = WideCharToMultiByte(CP_UTF8, 0, w, wlen, buf, n - 1, NULL, NULL);
    if (r < 0) r = 0;
    buf[r] = 0;
}

static const char *act(DWORD a)
{
    switch (a)
    {
    case FILE_ACTION_ADDED: return "ADD";
    case FILE_ACTION_REMOVED: return "DEL";
    case FILE_ACTION_MODIFIED: return "MOD";
    case FILE_ACTION_RENAMED_OLD_NAME: return "OLD";
    case FILE_ACTION_RENAMED_NEW_NAME: return "NEW";
    default: return "?";
    }
}

/* 届いた知らせを文字列に積む */
typedef struct
{
    char text[32768];
    int count;
    int calls;      /* 完了した読みの数 */
    int zero;       /* 0 バイトで完了した数 (あふれ) */
    DWORD lasterr;  /* 失敗の誤り */
} evlist;

static void addtext(evlist *ev, const char *s)
{
    size_t l = strlen(ev->text);
    if (l + strlen(s) + 2 < sizeof(ev->text))
    {
        if (l) strcat(ev->text, "|");
        strcat(ev->text, s);
    }
}

static void parse(const BYTE *buf, DWORD bytes, evlist *ev, int stamp)
{
    DWORD ofs = 0;
    ev->calls++;
    if (!bytes)
    {
        ev->zero++;
        addtext(ev, "(0)");
        return;
    }
    for (;;)
    {
        const FILE_NOTIFY_INFORMATION *fni = (const FILE_NOTIFY_INFORMATION *)(buf + ofs);
        char name[1024], line[1200];
        u8(fni->FileName, fni->FileNameLength / sizeof(WCHAR), name, sizeof(name));
        if (stamp)
            snprintf(line, sizeof(line), "%lu %s %s", (unsigned long)(GetTickCount() - t0), act(fni->Action), name);
        else
            snprintf(line, sizeof(line), "%s %s", act(fni->Action), name);
        addtext(ev, line);
        ev->count++;
        if (!fni->NextEntryOffset) break;
        ofs += fni->NextEntryOffset;
        if (ofs >= bytes) { addtext(ev, "(壊れた NextEntryOffset)"); break; }
    }
}

/* ---- 操作 ---- */

static void path(WCHAR *dst, const WCHAR *dir, const WCHAR *name)
{
    swprintf(dst, MAX_PATH, L"%ls\\%ls", dir, name);
}

static void op_create(const WCHAR *dir, const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    HANDLE h;
    path(p, dir, name);
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    else say("  ! create %ls: %lu\n", name, GetLastError());
}

static void op_write(const WCHAR *dir, const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    HANDLE h;
    DWORD w;
    path(p, dir, name);
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { say("  ! open %ls: %lu\n", name, GetLastError()); return; }
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, "hello\r\n", 7, &w, NULL);
    CloseHandle(h);
}

static void op_rename(const WCHAR *dir, const WCHAR *from, const WCHAR *to)
{
    WCHAR a[MAX_PATH], b[MAX_PATH];
    path(a, dir, from);
    path(b, dir, to);
    if (!MoveFileW(a, b)) say("  ! move %ls: %lu\n", from, GetLastError());
}

static void op_attr(const WCHAR *dir, const WCHAR *name, DWORD attr)
{
    WCHAR p[MAX_PATH];
    path(p, dir, name);
    if (!SetFileAttributesW(p, attr)) say("  ! attr %ls: %lu\n", name, GetLastError());
}

static void op_delete(const WCHAR *dir, const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    path(p, dir, name);
    if (!DeleteFileW(p)) say("  ! delete %ls: %lu\n", name, GetLastError());
}

static void op_mkdir(const WCHAR *dir, const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    path(p, dir, name);
    if (!CreateDirectoryW(p, NULL)) say("  ! mkdir %ls: %lu\n", name, GetLastError());
}

static void op_rmdir(const WCHAR *dir, const WCHAR *name)
{
    WCHAR p[MAX_PATH];
    path(p, dir, name);
    if (!RemoveDirectoryW(p)) say("  ! rmdir %ls: %lu\n", name, GetLastError());
}

#define GAP 60

/* 決まった順の操作。Windows と行ごとに比べる */
static void script(const WCHAR *dir)
{
    op_create(dir, L"a.txt");           Sleep(GAP);
    op_write(dir, L"a.txt");            Sleep(GAP);
    op_rename(dir, L"a.txt", L"b.txt"); Sleep(GAP);
    op_attr(dir, L"b.txt", FILE_ATTRIBUTE_READONLY); Sleep(GAP);
    op_attr(dir, L"b.txt", FILE_ATTRIBUTE_NORMAL);   Sleep(GAP);
    op_delete(dir, L"b.txt");           Sleep(GAP);
    op_mkdir(dir, L"sub");              Sleep(GAP);
    {
        WCHAR s[MAX_PATH];
        path(s, dir, L"sub");
        op_create(s, L"c.txt");         Sleep(GAP);
        op_write(s, L"c.txt");          Sleep(GAP);
        op_delete(s, L"c.txt");         Sleep(GAP);
    }
    op_rename(dir, L"sub", L"sub2");    Sleep(GAP);
    op_rmdir(dir, L"sub2");             Sleep(GAP);
    op_create(dir, L"\x65e5\x672c\x8a9e.txt"); Sleep(GAP);   /* 日本語.txt */
    op_delete(dir, L"\x65e5\x672c\x8a9e.txt"); Sleep(GAP);
}

/* 見張りの前からある下のフォルダー (pre\\deep) の中の変化 */
static void script_pre(const WCHAR *dir)
{
    WCHAR s[MAX_PATH];
    swprintf(s, MAX_PATH, L"%ls\\pre\\deep", dir);
    op_create(s, L"p.txt");             Sleep(GAP);
    op_write(s, L"p.txt");              Sleep(GAP);
    op_rename(s, L"p.txt", L"q.txt");   Sleep(GAP);
    op_delete(s, L"q.txt");             Sleep(GAP);
    op_mkdir(s, L"nd");                 Sleep(GAP);
    op_rmdir(s, L"nd");                 Sleep(GAP);
}

/* 名前を変えたフォルダー・外から移したフォルダーの中の変化 (見張りの外に out\\m\\deep を用意しておく) */
static WCHAR outside[MAX_PATH];
static void script_mv(const WCHAR *dir)
{
    WCHAR a[MAX_PATH], b[MAX_PATH];
    op_mkdir(dir, L"r1");                Sleep(GAP);
    op_rename(dir, L"r1", L"r2");        Sleep(GAP);
    swprintf(a, MAX_PATH, L"%ls\\r2", dir);
    op_create(a, L"x.txt");              Sleep(GAP);
    swprintf(a, MAX_PATH, L"%ls\\m", outside);
    swprintf(b, MAX_PATH, L"%ls\\m", dir);
    if (!MoveFileW(a, b)) say("  ! move in: %lu\n", GetLastError());
    Sleep(GAP);
    swprintf(a, MAX_PATH, L"%ls\\m\\deep", dir);
    op_create(a, L"y.txt");              Sleep(GAP);
    swprintf(a, MAX_PATH, L"%ls\\m", dir);
    swprintf(b, MAX_PATH, L"%ls\\m", outside);
    if (!MoveFileW(a, b)) say("  ! move out: %lu\n", GetLastError());
    Sleep(GAP);
    swprintf(a, MAX_PATH, L"%ls\\m\\deep", outside);
    op_create(a, L"z.txt");              Sleep(GAP);   /* 外に出した後: 届かないはず */
}

/* 前からある木 A\\C の名前を変えた直後に、その中で作る */
static void script_quick(const WCHAR *dir)
{
    WCHAR a[MAX_PATH];
    op_rename(dir, L"A", L"B");
    swprintf(a, MAX_PATH, L"%ls\\B\\C", dir);
    op_create(a, L"f.txt");             Sleep(GAP);
}

static void (*cur_script)(const WCHAR *dir) = script;

typedef struct
{
    WCHAR dir[MAX_PATH];
    HANDLE done;
} opsarg;

static DWORD WINAPI ops_thread(void *p)
{
    opsarg *a = p;
    Sleep(100);
    cur_script(a->dir);
    SetEvent(a->done);
    return 0;
}

static HANDLE open_dir(const WCHAR *dir, BOOL overlapped)
{
    return CreateFileW(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | (overlapped ? FILE_FLAG_OVERLAPPED : 0), NULL);
}

static int casenum;

static void fresh_dir(WCHAR *dir, const char *name)
{
    swprintf(dir, MAX_PATH, L"%ls\\c%02d", base, ++casenum);
    CreateDirectoryW(dir, NULL);
}

static void report(const char *name, evlist *ev)
{
    say("CASE %s: %s\n", name, ev->text[0] ? ev->text : "(なし)");
    if (ev->lasterr) say("  err=%lu\n", ev->lasterr);
}

/* 読み方 */
enum { M_OVL, M_IOCP, M_APC, M_SYNC };

static evlist *apc_ev;
static BYTE *apc_buf;
static volatile LONG apc_fired;
static DWORD apc_err;

static VOID CALLBACK apc_done(DWORD err, DWORD bytes, LPOVERLAPPED ov)
{
    apc_err = err;
    if (!err) parse(apc_buf, bytes, apc_ev, 0);
    InterlockedIncrement(&apc_fired);
}

typedef struct
{
    HANDLE h;
    DWORD filter;
    BOOL subtree;
    BYTE *buf;
    DWORD size;
    evlist *ev;
    volatile LONG stop;
} syncarg;

static DWORD WINAPI sync_reader(void *p)
{
    syncarg *a = p;
    while (!a->stop)
    {
        DWORD bytes = 0;
        if (!ReadDirectoryChangesW(a->h, a->buf, a->size, a->subtree, a->filter, &bytes, NULL, NULL))
        {
            if (!a->stop) a->ev->lasterr = GetLastError();
            break;
        }
        parse(a->buf, bytes, a->ev, 0);
    }
    return 0;
}

/* dir を見て、別の糸で script() を流し、届いた知らせを ev に集める */
static void run_rdcw(const WCHAR *dir, DWORD filter, BOOL subtree, int mode, DWORD bufsize, evlist *ev)
{
    HANDLE h = open_dir(dir, mode != M_SYNC);
    BYTE *buf = calloc(1, bufsize + 16);
    opsarg oa;
    HANDLE th;
    DWORD start;

    memset(ev, 0, sizeof(*ev));
    if (h == INVALID_HANDLE_VALUE) { ev->lasterr = GetLastError(); free(buf); return; }
    wcscpy(oa.dir, dir);
    oa.done = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (mode == M_SYNC)
    {
        syncarg sa = { h, filter, subtree, buf, bufsize, ev, 0 };
        HANDLE rt = CreateThread(NULL, 0, sync_reader, &sa, 0, NULL);
        Sleep(200);
        th = CreateThread(NULL, 0, ops_thread, &oa, 0, NULL);
        WaitForSingleObject(oa.done, 20000);
        Sleep(1500);
        sa.stop = 1;
        if (WaitForSingleObject(rt, 0) == WAIT_TIMEOUT)
        {
            if (!CancelSynchronousIo(rt)) say("  CancelSynchronousIo: %lu\n", GetLastError());
            if (WaitForSingleObject(rt, 3000) == WAIT_TIMEOUT)
            {
                say("  同期の読みが戻らない (糸を止める)\n");
                TerminateThread(rt, 1);
            }
        }
        CloseHandle(rt);
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
        CloseHandle(h);
        CloseHandle(oa.done);
        free(buf);
        return;
    }

    {
        OVERLAPPED ov;
        HANDLE port = NULL;
        int opsdone = 0;
        memset(&ov, 0, sizeof(ov));
        if (mode == M_OVL) ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (mode == M_IOCP) port = CreateIoCompletionPort(h, NULL, 0x1234, 1);
        if (mode == M_APC) { apc_ev = ev; apc_buf = buf; apc_fired = 0; apc_err = 0; }

        th = NULL;
        start = GetTickCount();
        for (;;)
        {
            BOOL ok;
            DWORD bytes = 0;
            if (ov.hEvent) ResetEvent(ov.hEvent);
            ok = ReadDirectoryChangesW(h, buf, bufsize, subtree, filter, NULL, &ov, mode == M_APC ? apc_done : NULL);
            if (!ok) { ev->lasterr = GetLastError(); break; }
            if (!th) th = CreateThread(NULL, 0, ops_thread, &oa, 0, NULL);
            for (;;)
            {
                DWORD w;
                int got = 0;
                if (!opsdone && WaitForSingleObject(oa.done, 0) == WAIT_OBJECT_0) opsdone = 1;
                if (mode == M_OVL)
                {
                    w = WaitForSingleObject(ov.hEvent, 1500);
                    if (w == WAIT_OBJECT_0)
                    {
                        if (GetOverlappedResult(h, &ov, &bytes, FALSE)) parse(buf, bytes, ev, 0);
                        else { ev->lasterr = GetLastError(); parse(buf, 0, ev, 0); }
                        got = 1;
                    }
                }
                else if (mode == M_IOCP)
                {
                    ULONG_PTR key;
                    OVERLAPPED *pov;
                    if (GetQueuedCompletionStatus(port, &bytes, &key, &pov, 1500)) { parse(buf, bytes, ev, 0); got = 1; }
                    else if (pov) { ev->lasterr = GetLastError(); parse(buf, 0, ev, 0); got = 1; }
                }
                else
                {
                    LONG before = apc_fired;
                    SleepEx(1500, TRUE);
                    if (apc_fired != before) { if (apc_err) { ev->lasterr = apc_err; } got = 1; }
                }
                if (got) break;
                if (opsdone || GetTickCount() - start > 25000)
                {
                    /* 静かになった: 取り消して終わる */
                    CancelIo(h);
                    if (mode == M_OVL) WaitForSingleObject(ov.hEvent, 2000);
                    else if (mode == M_IOCP) { ULONG_PTR key; OVERLAPPED *pov; GetQueuedCompletionStatus(port, &bytes, &key, &pov, 2000); }
                    else SleepEx(500, TRUE);
                    goto done;
                }
            }
            if (ev->lasterr && ev->lasterr != ERROR_NOTIFY_ENUM_DIR) break;
        }
done:
        if (th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); }
        if (ov.hEvent) CloseHandle(ov.hEvent);
        if (port) CloseHandle(port);
    }
    CloseHandle(h);
    CloseHandle(oa.done);
    free(buf);
}

/* ---- FindFirstChangeNotification ---- */

typedef void (*opfn)(const WCHAR *dir);
static void f_create(const WCHAR *d) { op_create(d, L"n.txt"); }
static void f_write(const WCHAR *d) { op_write(d, L"e.txt"); }
static void f_attr(const WCHAR *d) { op_attr(d, L"e.txt", FILE_ATTRIBUTE_HIDDEN); }
static void f_mkdir(const WCHAR *d) { op_mkdir(d, L"nd"); }
static void f_subcreate(const WCHAR *d) { WCHAR s[MAX_PATH]; path(s, d, L"es"); op_create(s, L"n.txt"); }
static void f_rename(const WCHAR *d) { op_rename(d, L"e.txt", L"e2.txt"); }
static void f_delete(const WCHAR *d) { op_delete(d, L"e.txt"); }

static void fcn_cases(void)
{
    static const struct { const char *name; DWORD f; } filters[] = {
        { "FILE_NAME", FILE_NOTIFY_CHANGE_FILE_NAME },
        { "DIR_NAME", FILE_NOTIFY_CHANGE_DIR_NAME },
        { "ATTRIBUTES", FILE_NOTIFY_CHANGE_ATTRIBUTES },
        { "SIZE", FILE_NOTIFY_CHANGE_SIZE },
        { "LAST_WRITE", FILE_NOTIFY_CHANGE_LAST_WRITE },
    };
    static const struct { const char *name; opfn fn; } ops[] = {
        { "create", f_create }, { "write", f_write }, { "attr", f_attr }, { "mkdir", f_mkdir },
        { "subcreate", f_subcreate }, { "rename", f_rename }, { "delete", f_delete },
    };
    int st, fi, oi;
    for (st = 0; st < 2; st++)
    {
        char line[1024] = "";
        for (fi = 0; fi < 5; fi++)
        {
            char row[256];
            snprintf(row, sizeof(row), "%s%s:", fi ? " " : "", filters[fi].name);
            strcat(line, row);
            for (oi = 0; oi < 7; oi++)
            {
                WCHAR dir[MAX_PATH], sub[MAX_PATH];
                HANDLE h;
                DWORD w;
                fresh_dir(dir, "fcn");
                op_create(dir, L"e.txt");
                path(sub, dir, L"es");
                CreateDirectoryW(sub, NULL);
                Sleep(50);
                h = FindFirstChangeNotificationW(dir, st, filters[fi].f);
                if (h == INVALID_HANDLE_VALUE) { strcat(line, "E"); continue; }
                ops[oi].fn(dir);
                w = WaitForSingleObject(h, 700);
                strcat(line, w == WAIT_OBJECT_0 ? "1" : "0");
                FindCloseChangeNotification(h);
            }
        }
        say("CASE fcn subtree=%d (create,write,attr,mkdir,subcreate,rename,delete): %s\n", st, line);
    }

    /* 再び待つ: FindNextChangeNotification の後に 2 回目の変化 */
    {
        WCHAR dir[MAX_PATH];
        HANDLE h;
        char res[64];
        DWORD w1, w2, w3;
        fresh_dir(dir, "fcn2");
        h = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
        op_create(dir, L"x1.txt");
        w1 = WaitForSingleObject(h, 1000);
        FindNextChangeNotification(h);
        w2 = WaitForSingleObject(h, 300);   /* まだ変化なし: 0 のはず */
        op_create(dir, L"x2.txt");
        w3 = WaitForSingleObject(h, 1000);
        FindCloseChangeNotification(h);
        snprintf(res, sizeof(res), "%d%d%d", w1 == WAIT_OBJECT_0, w2 == WAIT_OBJECT_0, w3 == WAIT_OBJECT_0);
        say("CASE fcn-next (1回目,次の前,2回目): %s\n", res);
    }
}

/* 下のフォルダー A を LAST_WRITE で直接見ているところへ、親を FILE_NAME の subtree で見る。A の中の書き込みが A の見張りに届くか */
static void nested_case(void)
{
    WCHAR dir[MAX_PATH], a[MAX_PATH];
    HANDLE ha, hr;
    OVERLAPPED oa, orr;
    BYTE *ba = calloc(1, 4096), *br = calloc(1, 4096);
    evlist *ev = calloc(1, sizeof(*ev)), *evr = calloc(1, sizeof(*evr));
    DWORD bytes;

    fresh_dir(dir, "nested");
    path(a, dir, L"A");
    CreateDirectoryW(a, NULL);
    op_create(a, L"f.txt");
    Sleep(100);
    memset(&oa, 0, sizeof(oa)); oa.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    memset(&orr, 0, sizeof(orr)); orr.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ha = open_dir(a, TRUE);
    hr = open_dir(dir, TRUE);
    ReadDirectoryChangesW(ha, ba, 4096, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE, NULL, &oa, NULL);
    ReadDirectoryChangesW(hr, br, 4096, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME, NULL, &orr, NULL);
    Sleep(100);
    op_write(a, L"f.txt");
    if (WaitForSingleObject(oa.hEvent, 1500) == WAIT_OBJECT_0 && GetOverlappedResult(ha, &oa, &bytes, FALSE))
        parse(ba, bytes, ev, 0);
    report("nested A の LAST_WRITE (親の FILE_NAME subtree の後)", ev);
    op_create(a, L"g.txt");
    if (WaitForSingleObject(orr.hEvent, 1500) == WAIT_OBJECT_0 && GetOverlappedResult(hr, &orr, &bytes, FALSE))
        parse(br, bytes, evr, 0);
    report("nested 親の FILE_NAME subtree", evr);
    CancelIo(ha); CancelIo(hr);
    WaitForSingleObject(oa.hEvent, 1000); WaitForSingleObject(orr.hEvent, 1000);
    CloseHandle(ha); CloseHandle(hr); CloseHandle(oa.hEvent); CloseHandle(orr.hEvent);
    free(ba); free(br); free(ev); free(evr);
}

/* ---- self ---- */

static void self_test(void)
{
    static const struct { const char *name; DWORD f; } filters[] = {
        { "FILE_NAME", FILE_NOTIFY_CHANGE_FILE_NAME },
        { "DIR_NAME", FILE_NOTIFY_CHANGE_DIR_NAME },
        { "ATTRIBUTES", FILE_NOTIFY_CHANGE_ATTRIBUTES },
        { "SIZE", FILE_NOTIFY_CHANGE_SIZE },
        { "LAST_WRITE", FILE_NOTIFY_CHANGE_LAST_WRITE },
        { "CREATION", FILE_NOTIFY_CHANGE_CREATION },
    };
    const DWORD all = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                      FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    WCHAR dir[MAX_PATH];
    evlist *ev = malloc(sizeof(*ev));
    int i;
    DWORD t;

    say("操作: create a.txt, write a.txt, rename a.txt->b.txt, attr RO, attr NORMAL, delete b.txt, mkdir sub, "
        "create sub\\c.txt, write sub\\c.txt, delete sub\\c.txt, rename sub->sub2, rmdir sub2, create 日本語.txt, delete 日本語.txt\n");

    fresh_dir(dir, "ovl"); t = GetTickCount();
    run_rdcw(dir, all, TRUE, M_OVL, 65536, ev); report("ovl all subtree=1", ev);
    say("  経過 %lums 知らせ %d 読み %d\n", GetTickCount() - t, ev->count, ev->calls);
    fresh_dir(dir, "ovl"); run_rdcw(dir, all, FALSE, M_OVL, 65536, ev); report("ovl all subtree=0", ev);
    fresh_dir(dir, "iocp"); run_rdcw(dir, all, TRUE, M_IOCP, 65536, ev); report("iocp all subtree=1", ev);
    fresh_dir(dir, "apc"); run_rdcw(dir, all, TRUE, M_APC, 65536, ev); report("apc all subtree=1", ev);
    fresh_dir(dir, "sync"); run_rdcw(dir, all, TRUE, M_SYNC, 65536, ev); report("sync all subtree=1", ev);
    for (i = 0; i < 6; i++)
    {
        char name[64];
        snprintf(name, sizeof(name), "ovl %s subtree=1", filters[i].name);
        fresh_dir(dir, "filter");
        run_rdcw(dir, filters[i].f, TRUE, M_OVL, 65536, ev);
        report(name, ev);
    }
    /* 小さい受け皿: あふれの扱い */
    fresh_dir(dir, "small"); run_rdcw(dir, all, TRUE, M_OVL, 16, ev); report("ovl all bufsize=16", ev);
    say("  0 バイトの完了 %d\n", ev->zero);

    /* 見張りの前からある下のフォルダー */
    {
        WCHAR p[MAX_PATH];
        fresh_dir(dir, "pre");
        swprintf(p, MAX_PATH, L"%ls\\pre", dir); CreateDirectoryW(p, NULL);
        swprintf(p, MAX_PATH, L"%ls\\pre\\deep", dir); CreateDirectoryW(p, NULL);
        cur_script = script_pre;
        run_rdcw(dir, all, TRUE, M_OVL, 65536, ev); report("ovl all subtree=1 既存の下のフォルダー", ev);
        fresh_dir(dir, "pre");
        swprintf(p, MAX_PATH, L"%ls\\pre", dir); CreateDirectoryW(p, NULL);
        swprintf(p, MAX_PATH, L"%ls\\pre\\deep", dir); CreateDirectoryW(p, NULL);
        run_rdcw(dir, all, FALSE, M_OVL, 65536, ev); report("ovl all subtree=0 既存の下のフォルダー", ev);
        cur_script = script;
    }
    /* 名前の変更・移動 */
    {
        WCHAR p[MAX_PATH];
        fresh_dir(dir, "mv");
        swprintf(outside, MAX_PATH, L"%ls\\out%02d", base, casenum);
        CreateDirectoryW(outside, NULL);
        swprintf(p, MAX_PATH, L"%ls\\m", outside); CreateDirectoryW(p, NULL);
        swprintf(p, MAX_PATH, L"%ls\\m\\deep", outside); CreateDirectoryW(p, NULL);
        cur_script = script_mv;
        run_rdcw(dir, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, TRUE, M_OVL, 65536, ev);
        report("ovl FILE_NAME|DIR_NAME subtree=1 名前の変更と移動", ev);
        cur_script = script;
    }

    /* 名前の変更の直後 */
    {
        WCHAR p[MAX_PATH];
        fresh_dir(dir, "quick");
        swprintf(p, MAX_PATH, L"%ls\\A", dir); CreateDirectoryW(p, NULL);
        swprintf(p, MAX_PATH, L"%ls\\A\\C", dir); CreateDirectoryW(p, NULL);
        cur_script = script_quick;
        run_rdcw(dir, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, TRUE, M_OVL, 65536, ev);
        report("ovl FILE_NAME|DIR_NAME subtree=1 名前の変更の直後", ev);
        cur_script = script;
    }
    nested_case();

    fcn_cases();
    free(ev);
}

/* ---- watch: 外からの変更 ---- */

static void cpu_line(const char *label)
{
    FILETIME c, e, k, u;
    ULARGE_INTEGER kk, uu;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    say("CPU %s: kernel=%llums user=%llums\n", label, kk.QuadPart / 10000, uu.QuadPart / 10000);
}

static void watch_dir(const WCHAR *dir, BOOL subtree, DWORD seconds)
{
    const DWORD all = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                      FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    HANDLE h = open_dir(dir, TRUE);
    BYTE *buf = calloc(1, 65536);
    OVERLAPPED ov;
    evlist *ev = malloc(sizeof(*ev));
    DWORD start = GetTickCount();
    HANDLE fcn;
    int fcn_count = 0;

    memset(ev, 0, sizeof(*ev));
    if (h == INVALID_HANDLE_VALUE) { say("開けない: %lu\n", GetLastError()); return; }
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    {
        DWORD w0 = GetTickCount(), w1;
        fcn = FindFirstChangeNotificationW(dir, subtree, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
        w1 = GetTickCount();
        if (!ReadDirectoryChangesW(h, buf, 65536, subtree, all, NULL, &ov, NULL))
        {
            say("RDCW 失敗: %lu\n", GetLastError());
            return;
        }
        /* 1 つ目の見張りで wineserver が下のフォルダーを辿る */
        say("見張りの開始 FCN %lums RDCW %lums\n", w1 - w0, GetTickCount() - w1);
    }
    t0 = GetTickCount();
    cpu_line("start");
    say("READY\n");
    while (GetTickCount() - start < seconds * 1000)
    {
        HANDLE hs[2] = { ov.hEvent, fcn };
        DWORD w = WaitForMultipleObjects(fcn != INVALID_HANDLE_VALUE ? 2 : 1, hs, FALSE, 1000);
        if (w == WAIT_OBJECT_0)
        {
            DWORD bytes = 0;
            memset(ev, 0, sizeof(*ev));
            if (GetOverlappedResult(h, &ov, &bytes, FALSE)) parse(buf, bytes, ev, 1);
            else ev->lasterr = GetLastError();
            say("EV %s%s\n", ev->text, ev->lasterr ? " (誤り)" : "");
            if (ev->lasterr) say("  err=%lu\n", ev->lasterr);
            ResetEvent(ov.hEvent);
            if (!ReadDirectoryChangesW(h, buf, 65536, subtree, all, NULL, &ov, NULL))
            {
                say("RDCW 再発行の失敗: %lu\n", GetLastError());
                break;
            }
        }
        else if (w == WAIT_OBJECT_0 + 1)
        {
            fcn_count++;
            say("FCN %lu\n", (unsigned long)(GetTickCount() - t0));
            FindNextChangeNotification(fcn);
        }
    }
    cpu_line("end");
    say("FCN の合図 %d 回\n", fcn_count);
    CancelIo(h);
    WaitForSingleObject(ov.hEvent, 2000);
    CloseHandle(h);
    if (fcn != INVALID_HANDLE_VALUE) FindCloseChangeNotification(fcn);
    free(buf);
    free(ev);
}

static void big_test(const WCHAR *dir, int ndirs, DWORD seconds)
{
    int i, made = 0;
    DWORD t = GetTickCount();
    int per = 100;
    for (i = 0; i < ndirs; i++)
    {
        WCHAR p[MAX_PATH];
        if (i % per == 0)
        {
            swprintf(p, MAX_PATH, L"%ls\\g%04d", dir, i / per);
            CreateDirectoryW(p, NULL);
        }
        swprintf(p, MAX_PATH, L"%ls\\g%04d\\d%05d", dir, i / per, i);
        if (CreateDirectoryW(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) made++;
    }
    say("フォルダー %d 個 (%lums)\n", made, GetTickCount() - t);
    watch_dir(dir, TRUE, seconds);
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR mode[32] = L"self", outp[MAX_PATH], tmp[64];
    DWORD seconds = 30;
    BOOL subtree = TRUE;
    int ndirs = 10000;

    out = stdout;
    if (GetEnvironmentVariableW(L"RDCW_OUT", outp, MAX_PATH))
    {
        FILE *f = _wfopen(outp, L"w");
        if (f) out = f;
    }
    if (GetEnvironmentVariableW(L"RDCW_MODE", tmp, 32)) wcscpy(mode, tmp);
    if (argc > 1) wcsncpy(mode, argv[1], 31);
    if (!GetEnvironmentVariableW(L"RDCW_DIR", base, MAX_PATH)) wcscpy(base, L"C:\\rdcwtest");
    if (argc > 2) wcsncpy(base, argv[2], MAX_PATH - 1);
    if (GetEnvironmentVariableW(L"RDCW_SECONDS", tmp, 32)) seconds = _wtoi(tmp);
    if (GetEnvironmentVariableW(L"RDCW_SUBTREE", tmp, 32)) subtree = _wtoi(tmp) != 0;
    if (GetEnvironmentVariableW(L"RDCW_NDIRS", tmp, 32)) ndirs = _wtoi(tmp);
    CreateDirectoryW(base, NULL);
    t0 = GetTickCount();
    say("RDCW_START mode=%ls dir=%ls\n", mode, base);

    if (!wcscmp(mode, L"self"))
    {
        /* 前の回の残りと混ざらないよう、回ごとに新しい場所 */
        WCHAR run[MAX_PATH];
        swprintf(run, MAX_PATH, L"%ls\\run%lu", base, GetTickCount());
        CreateDirectoryW(run, NULL);
        wcscpy(base, run);
        self_test();
    }
    else if (!wcscmp(mode, L"watch"))
        watch_dir(base, subtree, seconds);
    else if (!wcscmp(mode, L"idle"))
    {
        /* 比べ: 見ないで待つだけ */
        cpu_line("start");
        say("READY\n");
        Sleep(seconds * 1000);
        cpu_line("end");
    }
    else if (!wcscmp(mode, L"big"))
        big_test(base, ndirs, seconds);
    say("RDCW_RESULT done\n");
    if (out != stdout) fclose(out);
    return 0;
}
