/* NtContinue の拡張領域の長さの受け入れ条件を、本物のWindowsと比べる。 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
__declspec(dllimport) LONG NTAPI NtContinue(CONTEXT *,BOOLEAN);
__declspec(dllimport) ULONG64 NTAPI RtlGetEnabledExtendedFeatures(ULONG64);
typedef struct {LONG offset;ULONG length;} chunk;
static volatile LONG pass,returned,status;
static unsigned length,mask;
static DWORD WINAPI worker(void *unused)
{
    __attribute__((aligned(64))) unsigned char storage[4096];
    CONTEXT *ctx=(CONTEXT *)storage;chunk *ex=(chunk *)(ctx+1);
    unsigned char *xs=(unsigned char *)(((uintptr_t)(ex+3)+63)&~(uintptr_t)63);
    (void)unused;ZeroMemory(storage,sizeof(storage));RtlCaptureContext(ctx);
    if(InterlockedIncrement(&pass)==1)
    {
        ex[0].offset=-(LONG)sizeof(*ctx);ex[0].length=(ULONG)(xs-storage)+length;
        ex[1].offset=-(LONG)sizeof(*ctx);ex[1].length=sizeof(*ctx);
        ex[2].offset=(LONG)(xs-(unsigned char *)ex);ex[2].length=length;
        *(ULONG64 *)xs=mask;ctx->ContextFlags=CONTEXT_FULL|0x40;
        status=NtContinue(ctx,FALSE);returned=1;
    }
    return 0;
}
static DWORD WINAPI watchdog(void *p){(void)p;Sleep(30000);ExitProcess(3);}
int main(void)
{
    const unsigned lengths[]={0,1,32,63,64,319,320,321,512};
    setvbuf(stdout,NULL,_IONBF,0);CreateThread(NULL,0,watchdog,NULL,0,NULL);
    printf("context_size=%u enabled=%llx avx=%u\n",(unsigned)sizeof(CONTEXT),(unsigned long long)RtlGetEnabledExtendedFeatures(~0ULL),IsProcessorFeaturePresent(39));
    for(mask=0;mask<=4;mask+=4)for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++)
    {
        DWORD code;HANDLE h;length=lengths[i];pass=returned=0;status=0xdead;
        h=CreateThread(NULL,0,worker,NULL,0,NULL);
        if(WaitForSingleObject(h,3000)!=WAIT_OBJECT_0)return 4;
        GetExitCodeThread(h,&code);CloseHandle(h);
        printf("length=%u mask=%u resumed=%u returned=%ld status=%08lx exit=%lu\n",length,mask,pass==2,returned,(ULONG)status,code);
    }
    return 0;
}
