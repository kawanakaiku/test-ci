/* ARM64EC の停止予約中に、各待ちが自動で起きるかを測る自作の試験。
 * Microsoft のコード・DLL は含めない。 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
__declspec(dllimport) LONG NTAPI NtWaitForAlertByThreadId(const void *, const LARGE_INTEGER *);
__declspec(dllimport) LONG NTAPI NtWaitForSingleObject(HANDLE, BOOLEAN, const LARGE_INTEGER *);
__declspec(dllimport) LONG NTAPI NtDelayExecution(BOOLEAN, const LARGE_INTEGER *);
__declspec(dllimport) LONG NTAPI NtSetEvent(HANDLE, LONG *);
__declspec(dllimport) LONG NTAPI NtContinue(CONTEXT *, BOOLEAN);
static volatile LONG ready, pass;
static volatile unsigned char *area;
static volatile ULONG *bell;
static volatile LONG wait_status;
static volatile ULONG observed_bell;
static volatile LONGLONG wait_start, wait_end;
static LARGE_INTEGER freq;
static HANDLE event;
static LONGLONG now(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart * 1000000 / freq.QuadPart; }
static DWORD WINAPI watchdog(void *p) { (void)p; Sleep(90000); puts("WATCHDOG"); fflush(stdout); ExitProcess(3); }
static DWORD WINAPI worker(void *arg)
{
    CONTEXT ctx;
    LARGE_INTEGER timeout;
    unsigned mode = (unsigned)(uintptr_t)arg;
    void *teb;
    __asm__("mov %0, x18" : "=r"(teb));
    area = *(unsigned char **)((char *)teb + 0x1788);
    if (!area) { ready = -1; return 1; }
    bell = *(ULONG **)(area + 0x20);
    if (!bell) { ready = -2; return 1; }
    timeout.QuadPart = -2000000; /* 200 ms */
    RtlCaptureContext(&ctx);
    if (InterlockedIncrement(&pass) != 1) return 0;
    area[(mode >> 2) & 1] = 1;
    wait_start = now();
    ready = 1;
    switch (mode & 3)
    {
    case 0:
        while (!*bell) YieldProcessor();
        /* 予約後も翻訳中のまま通知と既に成立する待ちを実行する。 */
        wait_status = NtSetEvent(event, NULL);
        if (!wait_status) wait_status = NtWaitForSingleObject(event, FALSE, &timeout);
        break;
    case 1: wait_status = NtWaitForAlertByThreadId(NULL, &timeout); break;
    case 2: wait_status = NtWaitForSingleObject(event, FALSE, &timeout); break;
    case 3: wait_status = NtDelayExecution(FALSE, &timeout); break;
    }
    wait_end = now();
    observed_bell = *bell;
    area[0] = area[1] = 0;
    NtContinue(&ctx, FALSE);
    ExitProcess(4);
}
int main(int argc, char **argv)
{
    CONTEXT *extended=NULL; void *extended_mem=NULL; DWORD extended_size=0, capture_flags=CONTEXT_FULL;
    (void)argv;
    if (argc>1) {
        InitializeContext(NULL, CONTEXT_FULL|0x100040, NULL, &extended_size);
        extended_mem=malloc(extended_size);
        if (!extended_mem || !InitializeContext(extended_mem, CONTEXT_FULL|0x100040, &extended, &extended_size)) return 11;
        /* 未対応ならInitializeContextがXSTATEを外す。その返却フラグに従う。 */
        capture_flags=extended->ContextFlags;
    }
    unsigned bad=0;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (extended) printf("INIT_CONTEXT requested=%08lx actual=%08lx size=%lu\n",(DWORD)(CONTEXT_FULL|0x100040),capture_flags,extended_size);
    QueryPerformanceFrequency(&freq);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    for (unsigned mode = 0; mode < 8; mode++) for (unsigned i = 0; i < 5; i++)
    {
        HANDLE th; CONTEXT ctx, *captured=extended ? extended : &ctx; LONGLONG a,b,c; DWORD prev, error; BOOL ok;
        ready = pass = 0; wait_start=wait_end=0; wait_status = 0xdead; observed_bell = 0;
        th = CreateThread(NULL, 0, worker, (void *)(uintptr_t)mode, 0, NULL);
        while (!ready) Sleep(1);
        if (ready < 0) { printf("no area/bell %ld\n",ready); return 2; }
        Sleep(10);
        captured->ContextFlags = capture_flags;
        a=now(); prev=SuspendThread(th); b=now();
        ok=GetThreadContext(th,captured); error=ok ? 0 : GetLastError(); c=now();
        printf("mode=%u iter=%u suspend_us=%lld context_us=%lld prev=%lu ok=%d error=%lu status=%08lx wait_us=%lld bell=%08lx after=%08lx sim=%u callback=%u rip=%llx\n",
               mode,i,b-a,c-b,prev,ok,error,(ULONG)wait_status,wait_end-wait_start,observed_bell,*bell,area[0],area[1],(unsigned long long)(ok ? captured->Rip : 0));
        if (!ok || prev != 0 || wait_end <= wait_start || area[0] || area[1]) bad++;
        ResumeThread(th);
        if (WaitForSingleObject(th,3000)!=WAIT_OBJECT_0) { puts("JOIN_TIMEOUT"); return 5; }
        CloseHandle(th);
    }
    printf("DONE bad=%u xstate=%u actual_xstate=%u\n",bad,extended!=NULL,(capture_flags&0x40)!=0); free(extended_mem); return bad ? 10 : 0;
}
