/* CPU領域のdoorbellを自前のULONGへ一時的に向け、停止予約時と解除時の他ビットを測る。
 * 実際の翻訳器の内部フラグは書き換えない。ARM64ECの自作コードだけで実行する。 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
__declspec(dllimport) LONG NTAPI NtContinue(CONTEXT *, BOOLEAN);
static volatile ULONG synthetic, requested;
static volatile LONG ready, pass;
static unsigned which_flag;
static DWORD WINAPI watchdog(void *p) { (void)p;Sleep(90000);puts("WATCHDOG");fflush(stdout);ExitProcess(3); }
static DWORD WINAPI worker(void *arg)
{
    CONTEXT ctx;
    void *teb;unsigned char *area;
    ULONG **ptr,*old;
    (void)arg;
    __asm__("mov %0, x18" : "=r"(teb));
    area=*(unsigned char **)((char *)teb+0x1788);
    if(!area) { ready=-1;return 1; }
    ptr=(ULONG **)(area+0x20);old=*ptr;
    if(!old) {ready=-2;return 1;}
    RtlCaptureContext(&ctx);
    if(InterlockedIncrement(&pass)==1)
    {
        *ptr=(ULONG *)&synthetic;
        area[which_flag]=1;
        ready=1;
        while(!(synthetic&1)) YieldProcessor();
        requested=synthetic;
        area[0]=area[1]=0;
        NtContinue(&ctx,FALSE);
        ExitProcess(4);
    }
    *ptr=old;
    return 0;
}
int main(void)
{
    const ULONG masks[]={0,0x20,0x80000000,0x80000022};
    unsigned bad=0;
    setvbuf(stdout,NULL,_IONBF,0);
    CreateThread(NULL,0,watchdog,NULL,0,NULL);
    for(which_flag=0;which_flag<2;which_flag++) for(unsigned i=0;i<4;i++)
    {
        HANDLE h;CONTEXT ctx;BOOL ok;DWORD prev;ULONG after;
        ready=pass=0;requested=0;synthetic=masks[i];
        h=CreateThread(NULL,0,worker,NULL,0,NULL);
        while(!ready)Sleep(1);
        if(ready<0){printf("NO_AREA %ld\n",ready);return 2;}
        prev=SuspendThread(h);ctx.ContextFlags=CONTEXT_FULL;ok=GetThreadContext(h,&ctx);after=synthetic;
        printf("flag=%u initial=%08lx requested=%08lx after=%08lx prev=%lu ok=%d\n",which_flag,masks[i],requested,after,prev,ok);
        if(prev!=0 || !ok)bad++;
        ResumeThread(h);
        if(WaitForSingleObject(h,3000)!=WAIT_OBJECT_0)return 5;
        CloseHandle(h);
    }
    printf("DONE bad=%u\n",bad);return bad?10:0;
}
