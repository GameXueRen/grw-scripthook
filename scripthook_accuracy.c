/* Player-only accuracy control, ported from the Wildlands Immersion
 * Suite. The three sites are verified byte-for-byte before patching
 * and restored byte-for-byte on disable; the addresses are those of
 * the 2026-09-15 title update (build 25120584), where all three
 * signatures still match.
 *
 * The hooks are mid-function overwrites of 14-17 bytes that clobber
 * SIMD registers, so they are written as GCC naked functions - the
 * exact form shipped and field-tested in the Suite. The MSVC build
 * compiles a stub that refuses politely; porting the hooks to emitted
 * stubs is the follow-up if the MSVC path ever needs the feature.
 */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "image.h"
#include "log.h"

#define REF_SITE    SH_IMG(0x13BECE00)
#define ACC_SITE    SH_IMG(0x1471E06D)
#define BULLET_SITE SH_IMG(0x147EA558)
static const uint8_t refOrig[14]={0x40,0x53,0x48,0x83,0xEC,0x30,0x0F,0x29,0x74,0x24,0x20,0x48,0x89,0xCB};
static const uint8_t accOrig[14]={0x80,0xBB,0xF4,0,0,0,0,0xC6,0x83,0xE4,0,0,0,1};
static const uint8_t bulletOrig[17]={0xF3,0x0F,0x10,0x47,0x30,0x48,0x89,0xF9,0xF3,0x0F,0x59,0x47,0x28,0xF3,0x0F,0x59,0xC1};

#if defined(__GNUC__)

#include <math.h>

uint64_t g_refs[5] __attribute__((used));
uint64_t g_refReturn __attribute__((used)),g_accReturn __attribute__((used)),g_bulletReturn __attribute__((used));
static int g_enabled,g_refInstalled;
static volatile LONG g_applyAccuracy=1;
extern volatile LONG g_velocityMilli;   /* scripthook_hit.c */
const float g_accuracyVelocityEpsilon __attribute__((used))=0.000001f;

extern void ShSetError(int err);

/* Optional mod-owned pointer walks must fail safely when a weapon retires.
 * A numeric address threshold alone does not establish that memory exists. */
static uint64_t ReadPointer(uint64_t at){
    uint64_t value=0; SIZE_T got=0;
    if(at<0x10000 || at>0x00007fffffffffffULL-8)return 0;
    return ReadProcessMemory(GetCurrentProcess(),(void*)(uintptr_t)at,&value,8,&got)&&got==8?value:0;
}
static int WriteValue(uint64_t at,const void *value,SIZE_T len){
    SIZE_T wrote=0;
    if(at<0x10000 || at>0x00007fffffffffffULL-len)return 0;
    return WriteProcessMemory(GetCurrentProcess(),(void*)(uintptr_t)at,value,len,&wrote)&&wrote==len;
}
static SRWLOCK g_accuracyRefsLock=SRWLOCK_INIT;
static void __attribute__((used,noinline)) CaptureReference(uint64_t source){
    uint64_t owner=ReadPointer(source+16);
    if(!owner)return;
    AcquireSRWLockExclusive(&g_accuracyRefsLock);
    if(source!=g_refs[0]){
        g_refs[1]=g_refs[0];g_refs[3]=g_refs[2];
        g_refs[0]=source;g_refs[2]=owner;g_refs[4]=0;
    }else if(owner!=g_refs[2]){
        g_refs[2]=owner;g_refs[4]=0;
    }
    ReleaseSRWLockExclusive(&g_accuracyRefsLock);
}
static void __attribute__((used,noinline)) ApplyAccuracyContext(uint64_t context,uint64_t spread){
    uint64_t handle,object,owner,p,ref0,ref1;
    BYTE one=1; const uint32_t limits[4]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    AcquireSRWLockExclusive(&g_accuracyRefsLock);
    g_refs[4]=0;
    ref0=g_refs[2];ref1=g_refs[3];
    ReleaseSRWLockExclusive(&g_accuracyRefsLock);
    if(!InterlockedCompareExchange(&g_applyAccuracy,0,0))return;
    handle=ReadPointer(context+0xa8);
    if(!handle || !(object=ReadPointer(handle)))return;
    p=ReadPointer(object+0x30);if(!p)return;
    p=ReadPointer(p);if(!p)return;
    owner=ReadPointer(p+0x10);
    if(!owner || (owner!=ref0 && owner!=ref1))return;
    if(!WriteValue(spread+0xf4,&one,1))return;
    AcquireSRWLockExclusive(&g_accuracyRefsLock);
    g_refs[4]=spread;
    ReleaseSRWLockExclusive(&g_accuracyRefsLock);
    /* Keep the existing accuracy limits, but resolve every link for this call.
     * RPM/WPM report an inaccessible retired object instead of faulting in a cave. */
    p=ReadPointer(object+0x118);if(!p)return;
    p=ReadPointer(p+0x220);if(!p)return;
    p=ReadPointer(p);if(!p)return;
    if(ReadPointer(context+0xa8)!=handle || ReadPointer(handle)!=object)return;
    WriteValue(p+0x20,limits,sizeof(limits));
}
/* Helpers obey the Windows x64 ABI. Preserve all volatile state at these
 * mid-function hooks, including SIMD registers, flags and arbitrary RSP alignment. */
#define SAVE_CONTEXT \
"pushfq\n\tpush %rax\n\tpush %rcx\n\tpush %rdx\n\tpush %r8\n\tpush %r9\n\tpush %r10\n\tpush %r11\n\t" \
"mov %rsp,%r11\n\tand $-16,%rsp\n\tsub $0x90,%rsp\n\tmov %r11,0x80(%rsp)\n\t" \
"movdqu %xmm0,0x20(%rsp)\n\tmovdqu %xmm1,0x30(%rsp)\n\tmovdqu %xmm2,0x40(%rsp)\n\t" \
"movdqu %xmm3,0x50(%rsp)\n\tmovdqu %xmm4,0x60(%rsp)\n\tmovdqu %xmm5,0x70(%rsp)\n\t"
#define RESTORE_CONTEXT \
"movdqu 0x20(%rsp),%xmm0\n\tmovdqu 0x30(%rsp),%xmm1\n\tmovdqu 0x40(%rsp),%xmm2\n\t" \
"movdqu 0x50(%rsp),%xmm3\n\tmovdqu 0x60(%rsp),%xmm4\n\tmovdqu 0x70(%rsp),%xmm5\n\t" \
"mov 0x80(%rsp),%rsp\n\tpop %r11\n\tpop %r10\n\tpop %r9\n\tpop %r8\n\tpop %rdx\n\tpop %rcx\n\tpop %rax\n\tpopfq\n\t"
__attribute__((naked)) static void RefHook(void){__asm__ volatile(
SAVE_CONTEXT "mov %rbx,%rcx\n\tcall CaptureReference\n\t" RESTORE_CONTEXT
"push %rbx\n\tsub $0x30,%rsp\n\tmovaps %xmm6,0x20(%rsp)\n\tmov %rcx,%rbx\n\tjmp *g_refReturn(%rip)\n");}
__attribute__((naked)) static void AccuracyHook(void){__asm__ volatile(
SAVE_CONTEXT "mov %rdi,%rcx\n\tmov %rbx,%rdx\n\tcall ApplyAccuracyContext\n\t" RESTORE_CONTEXT
"cmpb $0,0xf4(%rbx)\n\tmovb $1,0xe4(%rbx)\n\tjmp *g_accReturn(%rip)\n");}

__attribute__((naked)) static void BulletHook(void){__asm__ volatile(
"pushfq\n\tmovss 0x30(%rdi),%xmm0\n\tmov %rdi,%rcx\n\tpush %rax\n\tcmpl $0,g_applyAccuracy(%rip)\n\tje 1f\n\tlea g_refs(%rip),%rax\n\tcmp 32(%rax),%rdi\n\tjne 1f\n\tpop %rax\n\tcmpl $1000,g_velocityMilli(%rip)\n\tje 2f\n\tmovss g_accuracyVelocityEpsilon(%rip),%xmm0\n\tjmp 3f\n2:\txorps %xmm0,%xmm0\n3:\tpopfq\n\tmulss %xmm1,%xmm0\n\tjmp *g_bulletReturn(%rip)\n1:\tpop %rax\n\tpopfq\n\tmulss 0x28(%rdi),%xmm0\n\tmulss %xmm1,%xmm0\n\tjmp *g_bulletReturn(%rip)\n");}

static int Bytes(uint64_t at,const uint8_t*b,int n){return memcmp((void*)(uintptr_t)at,b,(size_t)n)==0;}
/* Register-neutral 14-byte absolute jump. The previous mov-rax/jmp-rax
 * form destroyed live RAX before the cave could preserve it and caused
 * the firing-path crash. */
static void Jump(uint8_t*out,void*fn,int n){memset(out,0x90,(size_t)n);out[0]=0xFF;out[1]=0x25;*(uint32_t*)(out+2)=0;*(uint64_t*)(out+6)=(uint64_t)(uintptr_t)fn;}
static int Write(uint64_t at,const uint8_t*b,int n){DWORD old;if(!VirtualProtect((void*)(uintptr_t)at,n,PAGE_EXECUTE_READWRITE,&old))return 0;memcpy((void*)(uintptr_t)at,b,(size_t)n);VirtualProtect((void*)(uintptr_t)at,n,old,&old);FlushInstructionCache(GetCurrentProcess(),(void*)(uintptr_t)at,n);return 1;}
static int Install(uint64_t at,const uint8_t*orig,int n,void*fn){uint8_t j[17];Jump(j,fn,n);if(Bytes(at,j,n))return 1;if(!Bytes(at,orig,n))return 0;return Write(at,j,n);}
static int Restore(uint64_t at,const uint8_t*orig,int n,void*fn){uint8_t j[17];Jump(j,fn,n);if(Bytes(at,j,n))return 1;if(!Bytes(at,j,n))return 0;return Write(at,orig,n);}

SH_API int ShSetSuperAccuracy(int enabled){
    if(!g_refReturn){g_refReturn=REF_SITE+14;g_accReturn=ACC_SITE+14;g_bulletReturn=BULLET_SITE+17;}
    if(enabled){
        if(!Install(REF_SITE,refOrig,14,RefHook))return 0;g_refInstalled=1;
        if(!Install(BULLET_SITE,bulletOrig,17,BulletHook)){Restore(REF_SITE,refOrig,14,RefHook);g_refInstalled=0;return 0;}
        if(!Install(ACC_SITE,accOrig,14,AccuracyHook)){Restore(BULLET_SITE,bulletOrig,17,BulletHook);Restore(REF_SITE,refOrig,14,RefHook);g_refInstalled=0;return 0;}
        g_enabled=1;return 1;
    }
    if(!Restore(ACC_SITE,accOrig,14,AccuracyHook))return 0;
    if(!Restore(BULLET_SITE,bulletOrig,17,BulletHook))return 0;
    if(g_refInstalled&&!Restore(REF_SITE,refOrig,14,RefHook))return 0;
    memset(g_refs,0,sizeof(g_refs));g_refInstalled=0;g_enabled=0;return 1;
}
SH_API int ShGetSuperAccuracy(void){return g_enabled;}
SH_API void ShSetSuperAccuracyActive(int active){InterlockedExchange(&g_applyAccuracy,active?1:0);}

#else /* !__GNUC__: MSVC path - stubbed until the hooks move to emitted stubs */

SH_API int ShSetSuperAccuracy(int enabled){
    (void)enabled;
    Log("accuracy: not built for the MSVC toolchain (see the file header)");
    return 0;
}
SH_API int ShGetSuperAccuracy(void){return 0;}
SH_API void ShSetSuperAccuracyActive(int active){(void)active;}

#endif
