/* ContextFlags=0 の NtContinue が、停止予約なしでも復元するかを比較する。自作ARM64ECコードのみ。 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
__declspec(dllimport) LONG NTAPI NtContinue(CONTEXT *,BOOLEAN);
static volatile LONG pass,returned,status;
static ULONG requested;
static unsigned use_area,emu_stack;
static DWORD WINAPI watchdog(void *p){(void)p;Sleep(30000);puts("WATCHDOG");fflush(stdout);ExitProcess(3);}
static DWORD WINAPI worker(void *p)
{
    CONTEXT stack_ctx,backup,*ctx,*area_ctx;void *teb;unsigned char *area;void *oldbase=NULL,*oldlimit=NULL;
    (void)p;
#ifdef __arm64ec__
    __asm__("mov %0, x18":"=r"(teb));
#else
    teb=NtCurrentTeb();
#endif
    area=*(unsigned char **)((char *)teb+0x1788);
    if(!area)return 2;
    area_ctx=*(CONTEXT **)(area+0x18);if(!area_ctx)return 2;
    ctx=use_area?area_ctx:&stack_ctx;
    if(emu_stack){oldbase=*(void **)(area+8);oldlimit=*(void **)(area+16);*(void **)(area+8)=*(void **)((char *)teb+8);*(void **)(area+16)=*(void **)((char *)teb+16);}
    if(use_area)backup=*area_ctx;
    RtlCaptureContext(ctx);
    if(InterlockedIncrement(&pass)==1)
    {
        ctx->ContextFlags=requested;
        status=NtContinue(ctx,FALSE);
        returned=1;
    }
    if(use_area)*area_ctx=backup;
    if(emu_stack){*(void **)(area+8)=oldbase;*(void **)(area+16)=oldlimit;}
    return 0;
}
int main(void)
{
    const ULONG flags[]={0,0x100000,CONTEXT_FULL};unsigned bad=0;
    setvbuf(stdout,NULL,_IONBF,0);CreateThread(NULL,0,watchdog,NULL,0,NULL);
    #ifdef __arm64ec__
    const unsigned modes=2;
#else
    const unsigned modes=1;
#endif
    for(emu_stack=0;emu_stack<2;emu_stack++)for(use_area=0;use_area<modes;use_area++)for(unsigned i=0;i<3;i++)
    {
        HANDLE h;DWORD exitcode;requested=flags[i];pass=returned=0;status=0xdead;
        h=CreateThread(NULL,0,worker,NULL,0,NULL);if(WaitForSingleObject(h,5000)!=WAIT_OBJECT_0)return 4;
        GetExitCodeThread(h,&exitcode);CloseHandle(h);
        printf("emustack=%u area=%u flags=%08lx resumed=%u returned=%ld status=%08lx exit=%lu\n",emu_stack,use_area,requested,pass==2,returned,(ULONG)status,exitcode);
        if(exitcode || (requested==CONTEXT_FULL && pass!=2))bad++;
    }
    printf("DONE bad=%u\n",bad);return bad?10:0;
}
