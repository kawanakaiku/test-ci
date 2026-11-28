/* CONTEXT_XSTATE が立っていて後ろに CONTEXT_EX が無い文脈を RtlRestoreContext・NtContinue に渡したとき、
 * 本物の Windows (x64 のコードは Prism) が続きへ戻すか、呼び出し元へ帰すか、例外にするかを確かめる。
 * x64-nullref の回 (wine 051b362b) の直しの前提の確かめ。.NET の CLR は例外の文脈の頭 (sizeof(CONTEXT)) だけを写し、
 * ContextFlags の CONTEXT_XSTATE を残したまま RtlRestoreContext(ctx, NULL) を呼ぶ。 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
__declspec(dllimport) LONG NTAPI NtContinue(CONTEXT *,BOOLEAN);
__declspec(dllimport) ULONG64 NTAPI RtlGetEnabledExtendedFeatures(ULONG64);
#ifndef CONTEXT_XSTATE
#define CONTEXT_XSTATE (CONTEXT_AMD64|0x40)
#endif
enum {SC_CAPTURE,SC_VEH};
static volatile LONG pass,returned,landed,raised;
static volatile LONG st;
static volatile DWORD raised_code,orig_flags;
static int scenario,use_nt,fill;
static __attribute__((aligned(64))) unsigned char buf[8192];
static CONTEXT *copyctx;

static void landing(void){landed=1;ExitThread(0);}
static void after_return(void){ExitThread(4);}

static LONG WINAPI veh(EXCEPTION_POINTERS *e)
{
    DWORD code=e->ExceptionRecord->ExceptionCode;
    if(code==EXCEPTION_ACCESS_VIOLATION&&scenario==SC_VEH&&!copyctx)
    {
        CONTEXT *c=(CONTEXT *)buf;
        orig_flags=e->ContextRecord->ContextFlags;
        memset(buf,fill,sizeof(buf));
        memcpy(c,e->ContextRecord,sizeof(CONTEXT));   /* CLR の CopyOSContext と同じく頭だけ */
        c->Rip=(DWORD64)landing;c->Rsp=((e->ContextRecord->Rsp-0x400)&~(DWORD64)15)-8;
        copyctx=c;
        if(use_nt)st=NtContinue(c,FALSE);else{RtlRestoreContext(c,NULL);}
        returned=1;   /* 帰ってきたら、例外の文脈を書き換えて抜ける */
        e->ContextRecord->Rip=(DWORD64)after_return;e->ContextRecord->Rsp=((e->ContextRecord->Rsp-0x400)&~(DWORD64)15)-8;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(code!=EXCEPTION_ACCESS_VIOLATION||scenario!=SC_VEH){raised=1;raised_code=code;ExitThread(2);}
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    if(scenario==SC_CAPTURE)
    {
        CONTEXT *c=(CONTEXT *)buf;
        memset(buf,fill,sizeof(buf));
        RtlCaptureContext(c);
        if(InterlockedIncrement(&pass)==1)
        {
            orig_flags=c->ContextFlags;
            c->ContextFlags=CONTEXT_FULL|CONTEXT_XSTATE;
            if(use_nt)st=NtContinue(c,FALSE);else RtlRestoreContext(c,NULL);
            returned=1;
        }
        return 0;
    }
    *(volatile int *)0=1;   /* null の書き込み */
    return 5;
}
static DWORD WINAPI watchdog(void *p){(void)p;Sleep(30000);printf("TIMEOUT\n");ExitProcess(3);}
int main(int argc,char **argv)
{
    int only=argc>1?atoi(argv[1]):-1,idx=-1;
    static const char *sn[]={"capture","veh-copy"};
    setvbuf(stdout,NULL,_IONBF,0);CreateThread(NULL,0,watchdog,NULL,0,NULL);
    AddVectoredExceptionHandler(1,veh);
    printf("context_size=%u enabled=%llx avx=%u\n",(unsigned)sizeof(CONTEXT),(unsigned long long)RtlGetEnabledExtendedFeatures(~0ULL),IsProcessorFeaturePresent(39));
    for(scenario=SC_CAPTURE;scenario<=SC_VEH;scenario++)for(use_nt=0;use_nt<=1;use_nt++)for(int f=0;f<2;f++)
    {
        DWORD code;HANDLE h;
        if(only>=0&&++idx!=only)continue;
        fill=f?0xcc:0;pass=returned=landed=raised=0;st=0xdead;raised_code=0;orig_flags=0;copyctx=NULL;
        h=CreateThread(NULL,0,worker,NULL,0,NULL);
        if(WaitForSingleObject(h,5000)!=WAIT_OBJECT_0){printf("%s %s fill=%02x HANG\n",sn[scenario],use_nt?"NtContinue":"RtlRestoreContext",fill);continue;}
        GetExitCodeThread(h,&code);CloseHandle(h);
        printf("%s %s fill=%02x orig_flags=%08lx resumed=%d returned=%ld status=%08lx raised=%ld code=%08lx exit=%lu\n",
               sn[scenario],use_nt?"NtContinue":"RtlRestoreContext",fill,(ULONG)orig_flags,
               (int)(scenario==SC_CAPTURE?pass==2:landed),returned,(ULONG)st,raised,(ULONG)raised_code,code);
    }
    return 0;
}
