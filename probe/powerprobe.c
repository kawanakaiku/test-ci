/* SetThreadExecutionState / PowerSetRequest の Windows の振る舞いを確かめる */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <d3d9.h>
#include <stdio.h>

static void sysstate(const char *tag)
{
    ULONG st = 0;
    LONG r = CallNtPowerInformation(SystemExecutionState, NULL, 0, &st, sizeof(st));
    printf("[%s] SystemExecutionState r=%ld state=0x%08lx\n", tag, r, st);
    fflush(stdout);
}

static void requests(const char *tag)
{
    sysstate(tag);
    printf("---- powercfg /requests (%s)\n", tag); fflush(stdout);
    system("powercfg /requests");
    fflush(stdout);
}

static HANDLE go, done;
static DWORD WINAPI t_display(void *arg)
{
    EXECUTION_STATE r = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    printf("thread: STES(CONT|DISPLAY) -> 0x%08lx\n", r);
    r = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    printf("thread: STES(CONT|DISPLAY|SYSTEM) -> 0x%08lx\n", r);
    fflush(stdout);
    SetEvent(done);
    WaitForSingleObject(go, INFINITE);
    return 0; /* 解かずに終わる */
}
static DWORD WINAPI t_cont_only(void *arg)
{
    EXECUTION_STATE r = SetThreadExecutionState(ES_CONTINUOUS);
    printf("threadB: STES(CONT) -> 0x%08lx\n", r);
    fflush(stdout);
    SetEvent(done);
    WaitForSingleObject(go, INFINITE);
    return 0;
}

static HANDLE mkreq(const wchar_t *why)
{
    REASON_CONTEXT ctx = {0};
    ctx.Version = POWER_REQUEST_CONTEXT_VERSION;
    ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    ctx.Reason.SimpleReasonString = (wchar_t *)why;
    return PowerCreateRequest(&ctx);
}

static int child(const char *mode)
{
    if (!strcmp(mode, "stes")) SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    else { HANDLE h = mkreq(L"probe-child"); PowerSetRequest(h, PowerRequestDisplayRequired); }
    printf("child %s set\n", mode); fflush(stdout);
    Sleep(4000);
    ExitProcess(0); /* 解かずに終わる */
}

static void run_child(const char *self, const char *mode)
{
    char cmd[MAX_PATH + 32];
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    snprintf(cmd, sizeof(cmd), "\"%s\" child %s", self, mode);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { printf("CreateProcess %lu\n", GetLastError()); return; }
    Sleep(1500);
    requests(mode);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    requests("after child exit");
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void fullscreen(void)
{
    WNDCLASSA wc = {0};
    HWND hwnd; IDirect3D9 *d3d; IDirect3DDevice9 *dev = NULL; D3DPRESENT_PARAMETERS pp = {0};
    D3DDISPLAYMODE mode; HRESULT hr; int i;
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "probe";
    RegisterClassA(&wc);
    hwnd = CreateWindowA("probe", "probe", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, NULL, NULL, wc.hInstance, NULL);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("no d3d9\n"); return; }
    IDirect3D9_GetAdapterDisplayMode(d3d, 0, &mode);
    pp.Windowed = FALSE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = mode.Width;
    pp.BackBufferHeight = mode.Height; pp.BackBufferFormat = mode.Format; pp.hDeviceWindow = hwnd;
    pp.FullScreen_RefreshRateInHz = mode.RefreshRate;
    hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    printf("d3d9 fullscreen %ux%u CreateDevice hr=0x%08lx\n", mode.Width, mode.Height, hr); fflush(stdout);
    if (FAILED(hr)) {
        hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_REF, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
        printf("d3d9 fullscreen REF hr=0x%08lx\n", hr); fflush(stdout);
    }
    for (i = 0; i < 30 && dev; i++) { MSG msg; while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1, 0); IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL); Sleep(16); }
    requests("d3d9 fullscreen");
    if (dev) IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    DestroyWindow(hwnd);
    requests("after d3d9");
}

int main(int argc, char **argv)
{
    HANDLE t, t2, h;
    EXECUTION_STATE r;
    if (argc > 2 && !strcmp(argv[1], "child")) return child(argv[2]);
    go = CreateEventA(NULL, TRUE, FALSE, NULL); done = CreateEventA(NULL, FALSE, FALSE, NULL);

    requests("start");
    /* main thread の最初の値 */
    r = SetThreadExecutionState(ES_SYSTEM_REQUIRED);
    printf("main: first STES(SYSTEM, no CONT) -> 0x%08lx\n", r);
    r = SetThreadExecutionState(ES_DISPLAY_REQUIRED);
    printf("main: STES(DISPLAY, no CONT) -> 0x%08lx\n", r);
    r = SetThreadExecutionState(0);
    printf("main: STES(0) -> 0x%08lx\n", r);
    r = SetThreadExecutionState(ES_CONTINUOUS);
    printf("main: STES(CONT) -> 0x%08lx\n", r);
    r = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
    printf("main: STES(CONT|DISPLAY) -> 0x%08lx\n", r);
    r = SetThreadExecutionState(ES_DISPLAY_REQUIRED);
    printf("main: STES(DISPLAY, no CONT, while CONT) -> 0x%08lx\n", r);
    requests("main CONT|DISPLAY then DISPLAY");
    r = SetThreadExecutionState(ES_CONTINUOUS);
    printf("main: STES(CONT) clear -> 0x%08lx\n", r);
    r = SetThreadExecutionState(0x12345);
    printf("main: STES(bad 0x12345) -> 0x%08lx err=%lu\n", r, GetLastError());
    fflush(stdout);
    requests("main cleared");

    /* 別のスレッドが立てて、解かずに終わる */
    t = CreateThread(NULL, 0, t_display, NULL, 0, NULL);
    WaitForSingleObject(done, INFINITE);
    /* もう 1 つのスレッドの ES_CONTINUOUS だけでは、前のスレッドの求めは消えないか */
    t2 = CreateThread(NULL, 0, t_cont_only, NULL, 0, NULL);
    WaitForSingleObject(done, INFINITE);
    requests("thread A CONT|DISPLAY|SYSTEM, thread B CONT");
    SetEvent(go);
    WaitForSingleObject(t, INFINITE); WaitForSingleObject(t2, INFINITE);
    CloseHandle(t); CloseHandle(t2);
    requests("after threads exit");
    ResetEvent(go);

    /* PowerSetRequest の数え方 */
    h = mkreq(L"probe-display");
    printf("PowerCreateRequest %p\n", h);
    printf("set1 %d\n", PowerSetRequest(h, PowerRequestDisplayRequired));
    printf("set2 %d\n", PowerSetRequest(h, PowerRequestDisplayRequired));
    printf("setSys %d\n", PowerSetRequest(h, PowerRequestSystemRequired));
    printf("clear1 %d\n", PowerClearRequest(h, PowerRequestDisplayRequired));
    requests("set2 clear1");
    printf("clear2 %d\n", PowerClearRequest(h, PowerRequestDisplayRequired));
    SetLastError(0);
    printf("clear3 %d err=%lu\n", PowerClearRequest(h, PowerRequestDisplayRequired), GetLastError());
    requests("set2 clear3");
    printf("set again %d\n", PowerSetRequest(h, PowerRequestDisplayRequired));
    CloseHandle(h);
    requests("after CloseHandle (display+system set)");

    run_child(argv[0], "stes");
    run_child(argv[0], "req");
    fullscreen();
    return 0;
}
