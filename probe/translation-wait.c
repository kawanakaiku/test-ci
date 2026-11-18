/* x64 の動的コードを並列に翻訳させ、実際に待機状態かつ InSimulation=1 のスレッドを選んで停止を測る。
 * コード生成はこの自作の算術列のみ。DLL やシステムの命令列は保存しない。 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define WORKERS 8
#define FUNCS 8192
#define STRIDE 1024
typedef LONG (NTAPI *query_thread_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
typedef LONG (NTAPI *query_system_fn)(ULONG,void *,ULONG,ULONG *);
static HANDLE threads[WORKERS];
static DWORD tids[WORKERS];
static volatile unsigned char *areas[WORKERS];
static volatile LONG stop;
static volatile LONG64 calls[WORKERS];
static LARGE_INTEGER freq;
static LONGLONG now(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart * 1000000 / freq.QuadPart; }
static DWORD WINAPI watchdog(void *p) { (void)p; Sleep(75000); puts("WATCHDOG"); fflush(stdout); ExitProcess(3); }
static DWORD WINAPI worker(void *arg)
{
    unsigned id=(unsigned)(uintptr_t)arg, seed=id+1;
    unsigned char *code=VirtualAlloc(NULL,FUNCS*STRIDE,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    if (!code) return 1;
    while (!stop)
    {
        for (unsigned f=0;f<FUNCS;f++)
        {
            unsigned char *p=code+f*STRIDE;
            *p++=0xb8; memcpy(p,&seed,4);p+=4;
            for (unsigned j=0;j<160;j++) { *p++=0x05;memcpy(p,&f,4);p+=4; }
            *p++=0xc3;
        }
        FlushInstructionCache(GetCurrentProcess(),code,FUNCS*STRIDE);
        for (unsigned f=0;f<FUNCS && !stop;f++)
        {
            unsigned (__cdecl *fn)(void)=(void *)(code+f*STRIDE);
            unsigned got=fn();
            if (got!=seed+f*160) { printf("BAD worker=%u f=%u got=%u expected=%u\n",id,f,got,seed+f*160);ExitProcess(6); }
            calls[id]++;
        }
        seed++;
    }
    VirtualFree(code,0,MEM_RELEASE);
    return 0;
}
int main(void)
{
    HMODULE nt=GetModuleHandleW(L"ntdll.dll");
    query_thread_fn query_thread=(void *)GetProcAddress(nt,"NtQueryInformationThread");
    query_system_fn query_system=(void *)GetProcAddress(nt,"NtQuerySystemInformation");
    unsigned char *buf=malloc(8*1024*1024); ULONG length=0;
    unsigned captures=0, waits=0, samples=0; LONGLONG max_compute=0,max_wait=0,start;
    setvbuf(stdout,NULL,_IONBF,0);QueryPerformanceFrequency(&freq);
    CreateThread(NULL,0,watchdog,NULL,0,NULL);
    for(unsigned i=0;i<WORKERS;i++)
    {
        uint64_t basic[6]={0};
        threads[i]=CreateThread(NULL,0,worker,(void *)(uintptr_t)i,0,&tids[i]);
        if (!threads[i] || query_thread(threads[i],0,basic,sizeof(basic),NULL)<0) return 2;
        areas[i]=*(unsigned char **)((char *)(uintptr_t)basic[1]+0x1788);
    }
    start=now();
    while (now()-start<20000000 && (waits<50 || captures<100))
    {
        unsigned state[WORKERS]={0},reason[WORKERS]={0};
        LONG status=query_system(5,buf,8*1024*1024,&length);
        if(status<0) { printf("QUERY status=%08lx length=%lu\n",status,length);return 7; }
        for (unsigned char *p=buf;;p+=*(ULONG *)p)
        {
            if (*(ULONG_PTR *)(p+0x50)==GetCurrentProcessId())
            {
                ULONG n=*(ULONG *)(p+4);
                for(unsigned j=0;j<n;j++)
                {
                    unsigned char *t=p+0x100+j*0x50;
                    for(unsigned i=0;i<WORKERS;i++) if(*(ULONG_PTR *)(t+0x30)==tids[i])
                    { state[i]=*(ULONG *)(t+0x44);reason[i]=*(ULONG *)(t+0x48); }
                }
                break;
            }
            if(!*(ULONG *)p) break;
        }
        samples++;
        for(unsigned i=0;i<WORKERS;i++)
        {
            CONTEXT ctx; LONGLONG a,b,c;DWORD prev;BOOL ok;
            unsigned sim=areas[i] ? areas[i][0] : 0;
            int iswait=(state[i]==5);
            if (!sim || (!iswait && captures>=100) || (iswait && waits>=100)) continue;
            if (iswait) printf("CANDIDATE worker=%u state=%u reason=%u sim=%u bell=%08lx calls=%lld\n",i,state[i],reason[i],sim,**(ULONG **)(areas[i]+0x20),calls[i]);
            ctx.ContextFlags=CONTEXT_FULL;
            a=now();prev=SuspendThread(threads[i]);b=now();ok=GetThreadContext(threads[i],&ctx);c=now();
            if(iswait) { waits++;if(c-b>max_wait)max_wait=c-b; }
            else { captures++;if(c-b>max_compute)max_compute=c-b; }
            if(iswait || !ok || prev!=0 || c-b>10000)
                printf("CAPTURE worker=%u state=%u reason=%u sim_before=%u sim_after=%u suspend_us=%lld context_us=%lld prev=%lu ok=%d rip=%llx\n",i,state[i],reason[i],sim,areas[i][0],b-a,c-b,prev,ok,(unsigned long long)ctx.Rip);
            if(prev!=(DWORD)-1) ResumeThread(threads[i]);
            if(!ok || prev!=0) return 8;
        }
    }
    stop=1;
    if (WaitForMultipleObjects(WORKERS,threads,TRUE,10000)!=WAIT_OBJECT_0) { puts("JOIN_TIMEOUT");return 9; }
    printf("DONE samples=%u compute=%u wait=%u max_compute_us=%lld max_wait_us=%lld\n",samples,captures,waits,max_compute,max_wait);
    return 0;
}
