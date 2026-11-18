/* 停止予約への切替中に成立したイベントを取りこぼさないかを測る。ARM64ECの自作スレッドのみ。 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
__declspec(dllimport) LONG NTAPI NtWaitForSingleObject(HANDLE,BOOLEAN,const LARGE_INTEGER *);
__declspec(dllimport) LONG NTAPI NtContinue(CONTEXT *,BOOLEAN);
static volatile LONG ready,pass,wait_status;
static HANDLE gate;
static LARGE_INTEGER freq;
static LONGLONG now(void){LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart*1000000/freq.QuadPart;}
static DWORD WINAPI watchdog(void *p){(void)p;Sleep(90000);puts("WATCHDOG");fflush(stdout);ExitProcess(3);}
static DWORD WINAPI worker(void *p)
{
    void *teb;volatile unsigned char *area;CONTEXT ctx;LARGE_INTEGER timeout;
    (void)p;__asm__("mov %0, x18":"=r"(teb));area=*(unsigned char **)((char *)teb+0x1788);
    if(!area){ready=-1;return 1;}
    timeout.QuadPart=-2000000;
    RtlCaptureContext(&ctx);
    if(InterlockedIncrement(&pass)!=1)return 0;
    area[0]=1;ready=1;
    wait_status=NtWaitForSingleObject(gate,FALSE,&timeout);
    area[0]=0;NtContinue(&ctx,FALSE);ExitProcess(4);
}
int main(void)
{
    unsigned bad=0,late=0;
    setvbuf(stdout,NULL,_IONBF,0);QueryPerformanceFrequency(&freq);CreateThread(NULL,0,watchdog,NULL,0,NULL);
    for(unsigned i=0;i<64;i++)
    {
        HANDLE h;CONTEXT ctx;DWORD prev;BOOL signaled,ok;LONGLONG a,b,c,d;
        ready=pass=0;wait_status=0xdead;gate=CreateEventW(NULL,FALSE,FALSE,NULL);
        h=CreateThread(NULL,0,worker,NULL,0,NULL);while(!ready)Sleep(1);
        if(ready<0)return 2;
        Sleep(2);ctx.ContextFlags=CONTEXT_FULL;
        a=now();prev=SuspendThread(h);b=now();signaled=SetEvent(gate);c=now();ok=GetThreadContext(h,&ctx);d=now();
        printf("iter=%u suspend_us=%lld signal_us=%lld context_us=%lld status=%08lx prev=%lu signaled=%d ok=%d\n",i,b-a,c-b,d-c,(ULONG)wait_status,prev,signaled,ok);
        if(c-a>150000)late++;
        else if(prev!=0 || !signaled || !ok || wait_status!=0)bad++;
        ResumeThread(h);if(WaitForSingleObject(h,2000)!=WAIT_OBJECT_0)return 5;
        CloseHandle(h);CloseHandle(gate);
    }
    printf("DONE bad=%u late=%u\n",bad,late);return bad?10:0;
}
