/* x64 program for Windows on ARM (Prism): watch the ARM64EC CPU area doorbell of a worker thread while another thread
   suspends it and reads its context, as a .NET GC does. Prints what the real kernel writes into the doorbell. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
typedef LONG (NTAPI *qit_fn)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG (NTAPI *setev_fn)(HANDLE, LONG *);
static volatile LONG stop, poll_stop;
static HANDLE evt;
static setev_fn pSetEvent;
static volatile ULONG64 counter;
static volatile uint32_t *bell;
static LARGE_INTEGER freq;
struct ev { LONGLONG t; uint32_t v; };
static struct ev evs[512];
static volatile LONG nev;
static LONGLONG now(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart * 1000000 / freq.QuadPart; }

static DWORD WINAPI worker(void *arg)
{
    int mode = (int)(intptr_t)arg;
    while (!stop)
    {
        if (mode == 1) pSetEvent(evt, NULL);
        else if (mode == 2) WaitForSingleObject(evt, 1);
        else if (mode == 3) Sleep(1);
        counter++;
    }
    return 0;
}
static DWORD WINAPI poller(void *arg)
{
    uint32_t last = *bell;
    (void)arg;
    evs[0].t = now(); evs[0].v = last; nev = 1;
    while (!poll_stop)
    {
        uint32_t v = *bell;
        if (v != last && nev < 512) { evs[nev].t = now(); evs[nev].v = v; nev++; last = v; }
    }
    return 0;
}
static DWORD WINAPI watchdog(void *arg)
{
    (void)arg; Sleep(90000);
    printf("WATCHDOG: hang, exiting\n"); fflush(stdout); ExitProcess(3);
}
static void show_file_version(const char *path)
{
    DWORD h, n = GetFileVersionInfoSizeA(path, &h);
    VS_FIXEDFILEINFO *fi; UINT len; char *buf;
    if (!n || !(buf = malloc(n))) { printf("version %s: none\n", path); return; }
    if (GetFileVersionInfoA(path, 0, n, buf) && VerQueryValueA(buf, "\\", (void **)&fi, &len))
        printf("version %s: %u.%u.%u.%u\n", path, HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    free(buf);
}
int main(void)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    qit_fn query = (qit_fn)GetProcAddress(nt, "NtQueryInformationThread");
    int mode;
    pSetEvent = (setev_fn)GetProcAddress(nt, "NtSetEvent");
    QueryPerformanceFrequency(&freq);
    setvbuf(stdout, NULL, _IONBF, 0);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    show_file_version("C:\\Windows\\System32\\xtajit64.dll");
    show_file_version("C:\\Windows\\System32\\ntdll.dll");
    show_file_version("C:\\Windows\\System32\\wow64.dll");
    evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (mode = 0; mode < 4; mode++)
    {
        HANDLE th; uint64_t info[6] = {0}; char *teb, *area; int i; HANDLE pt;
        stop = poll_stop = 0; nev = 0; counter = 0;
        th = CreateThread(NULL, 0, worker, (void *)(intptr_t)mode, 0, NULL);
        Sleep(300);
        if (query(th, 0, info, sizeof(info), NULL) < 0) { printf("query failed\n"); return 1; }
        teb = (char *)(uintptr_t)info[1];
        area = *(char **)(teb + 0x1788);
        printf("== mode %d (0=compute 1=NtSetEvent 2=Wait 3=Sleep) teb=%p area=%p\n", mode, teb, area);
        if (!area) { printf("no cpu area\n"); stop = 1; continue; }
        bell = *(uint32_t **)(area + 0x20);
        printf("InSimulation=%u InSyscallCallback=%u bell_ptr=%p bell=%08x\n", area[0], area[1], (void *)bell, bell ? *bell : 0);
        if (!bell) { stop = 1; continue; }
        pt = CreateThread(NULL, 0, poller, NULL, 0, NULL);
        SetThreadPriority(pt, THREAD_PRIORITY_HIGHEST);
        Sleep(50);
        for (i = 0; i < 15; i++)
        {
            LONGLONG t0, t1, t2, t3; DWORD len = 0; CONTEXT *ctx; void *mem; DWORD cnt;
            uint32_t b_before = *bell, b_after;
            unsigned insim_at_ctx;
            InitializeContext(NULL, CONTEXT_FULL | 0x100040, NULL, &len);
            mem = malloc(len);
            InitializeContext(mem, CONTEXT_FULL | 0x100040, &ctx, &len);
            t0 = now();
            cnt = SuspendThread(th);
            t1 = now();
            insim_at_ctx = (unsigned char)area[0];
            if (!GetThreadContext(th, ctx)) printf("GetThreadContext failed %lu\n", GetLastError());
            t2 = now();
            b_after = *bell;
            printf("iter %2d suspend=%lld us ctx=%lld us prev=%lu bell before=%08x after=%08x InSim(after suspend)=%u InSim(after ctx)=%u rip=%llx\n",
                   i, t1 - t0, t2 - t1, cnt, b_before, b_after, insim_at_ctx, (unsigned char)area[0], (unsigned long long)ctx->Rip);
            t3 = now(); (void)t3;
            ResumeThread(th);
            free(mem);
            Sleep(20);
        }
        poll_stop = 1; WaitForSingleObject(pt, 2000);
        printf("doorbell transitions seen by poller (%ld):\n", nev);
        for (i = 0; i < nev && i < 40; i++) printf("  +%lld us -> %08x\n", evs[i].t - evs[0].t, evs[i].v);
        stop = 1; WaitForSingleObject(th, 3000);
    }
    printf("DONE\n");
    return 0;
}
