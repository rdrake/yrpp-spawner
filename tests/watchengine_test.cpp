// WatchEngine under Wine; run by tests/run_watchengine_test.sh in three launch modes.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "WatchEngine.h"

using namespace WatchEngine;

static volatile int g_frame = 0;
static int __cdecl FrameNow() { return g_frame; }

struct Collected { Row rows[64]; int n; };
static void __cdecl Collect(const Row& r, void* ctx)
{
	Collected* c = static_cast<Collected*>(ctx);
	if (c->n < 64) c->rows[c->n++] = r;
}

static volatile unsigned int g_ret = 0;
__attribute__((noinline)) static void StoreIt(volatile unsigned int* p, unsigned int v)
{
	g_ret = (unsigned int)(uintptr_t)__builtin_return_address(0);
	*p = v;
}
__attribute__((noinline)) static void Caller(volatile unsigned int* p, unsigned int v) { StoreIt(p, v); }

struct Obj { unsigned int vtable; unsigned char pad[0x1C0]; unsigned int field; };  // field at +0x1C4

// Exec targets are hand-assembled so LEN is known, and live in their own
// section, off the engine's page (WatchEngine.h, LIMITS).
//   Hooked(a, o) = a + o->field: mov eax,[esp+4] (4); mov ecx,[esp+8] (4);
//     add eax,[ecx+0x1C4] (6); ret. LEN 8 copies the two movs.
//   StoreAtPoint(p, v): mov eax,[esp+4]; mov ecx,[esp+8]; storepoint:
//     mov [eax],ecx (2); nop x3; ret. LEN 5 copies the store, so a watched
//     store runs from the trampoline.
__asm__(
	".section .hookme,\"xr\"\n"
	".globl _Hooked\n_Hooked:\n"
	"  movl 4(%esp), %eax\n  movl 8(%esp), %ecx\n  addl 0x1C4(%ecx), %eax\n  ret\n"
	".globl _StoreAtPoint\n_StoreAtPoint:\n"
	"  movl 4(%esp), %eax\n  movl 8(%esp), %ecx\n"
	".globl _storepoint\n_storepoint:\n"
	"  movl %ecx, (%eax)\n  nop\n  nop\n  nop\n  ret\n"
	".text\n");
extern "C" int __cdecl Hooked(int a, Obj* o);
extern "C" void __cdecl StoreAtPoint(volatile unsigned int* p, unsigned int v);
extern "C" char storepoint[];
// The asm after the call keeps it a call, not a tail jmp, so s0 returns into here.
__attribute__((noinline)) static int CallHooked(int a, Obj* o) { const int r = Hooked(a, o); __asm__ volatile("" ::: "memory"); return r; }

// True when `ret` is the return address of a direct call to `callee`.
static bool ReturnsFromCallTo(unsigned int ret, unsigned int callee)
{
	const unsigned char* site = (const unsigned char*)(uintptr_t)(ret - 5);
	int rel = 0;
	memcpy(&rel, site + 1, 4);
	return site[0] == 0xE8 && ret + (unsigned int)rel == callee;
}

struct CollectedX { ExecRow rows[16]; int n; };
static void __cdecl CollectX(const ExecRow& r, void* ctx)
{
	CollectedX* c = static_cast<CollectedX*>(ctx);
	if (c->n < 16) c->rows[c->n++] = r;
}

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv)
{
	freopen("engtest.out", "w", stdout);
	setvbuf(stdout, nullptr, _IONBF, 0);
	volatile FARPROC keep = GetProcAddress(LoadLibraryA("kernel32.dll"), "GetTickCount"); (void)keep;
	__asm__ volatile(".globl hookpoint\nhookpoint: nop; nop; nop; nop; nop; nop; nop; nop");
	const bool expectFail = argc > 1 && !strcmp(argv[1], "expect-fail");

	unsigned char* data = (unsigned char*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	unsigned char* heapB = (unsigned char*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	unsigned char* heapC = (unsigned char*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	unsigned int dataAddr = (unsigned int)(uintptr_t)data;
	unsigned int slot = dataAddr + 0x230;        // like ScenarioClass::Instance
	unsigned int countries = dataAddr + 0x29C;   // like AISlots.Countries, 32 bytes

	char spec[128], err[256];
	snprintf(spec, sizeof spec, "0x%X:32, [%X]+214:4", countries, slot);
	const bool ok = Arm(spec, FrameNow, err, sizeof err);
	printf("arm=%d err=\"%s\" selftest=%d targets=%d text=%08X-%08X\n", ok, err, SelfTestHits(), TargetCount(), TextLow(), TextHigh());
	if (expectFail)
	{
		CHECK(!ok, "arm should fail when Syringe stays attached");
		CHECK(SelfTestHits() == 0, "selftest %d", SelfTestHits());
		// the game must keep running normally: nothing is protected
		*(volatile unsigned int*)(countries) = 7;
		printf("VERDICT: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
		return failures;
	}
	CHECK(ok, "arm failed: %s", err);
	CHECK(SelfTestHits() == 2, "selftest %d", SelfTestHits());
	CHECK(TargetCount() == 3, "targets %d", TargetCount());

	volatile unsigned int* c = (volatile unsigned int*)countries;
	g_frame = 1;
	c[0] = 11;                                  // row: off 0, 0 -> 11
	InterlockedIncrement((volatile LONG*)&c[3]);  // row: off 12, 0 -> 1
	unsigned int r = c[0]; (void)r;             // read: no row
	*(volatile unsigned int*)(dataAddr + 0x100) = 5;  // neighbour
	*(volatile unsigned int*)(heapB + 0x214) = 99;    // not yet watched
	g_frame = 2;
	*(volatile unsigned int*)slot = (unsigned int)(uintptr_t)heapB;   // Base row; resolves to heapB+0x214
	Caller((volatile unsigned int*)(heapB + 0x214), 100);             // Deref row 99 -> 100, candidate = return into Caller
	const unsigned int retIntoCaller = g_ret;
	*(volatile unsigned int*)(heapB + 0x300) = 1;     // neighbour on heapB
	*(volatile unsigned int*)(countries - 4) = 5;    // the field BELOW the range: neighbour, no row
	g_frame = 3;
	*(volatile unsigned int*)slot = (unsigned int)(uintptr_t)heapC;   // Base row; heapB released
	*(volatile unsigned int*)(heapB + 0x214) = 101;   // no longer watched, no trap
	*(volatile unsigned int*)(heapC + 0x214) = 7;     // Deref row 0 -> 7
	__asm__ volatile("cld; rep stosl" :: "D"(countries + 16), "a"(0xEE), "c"(4) : "memory"); // 4 rows off 16..28

	Collected got = {};
	Drain(Collect, &got);
	for (int i = 0; i < got.n; ++i)
	{
		const Row& w = got.rows[i];
		printf("W=%d,%08X,%08X,t%u,+%u,%u->%u,c:%08X,%08X\n", w.Frame, w.Eip, w.FaultAddress, w.Target, w.Offset, w.OldValue, w.NewValue, w.Candidates[0], w.Candidates[1]);
	}
	printf("rows=%d neighbour=%ld dropped=%ld\n", got.n, NeighbourTraps(), Dropped());

	CHECK(got.n == 10, "rows %d (want 10)", got.n);
	if (got.n == 10)
	{
		const Row* w = got.rows;
		CHECK(w[0].Target == 0 && w[0].Offset == 0 && w[0].OldValue == 0 && w[0].NewValue == 11 && w[0].Frame == 1, "row0");
		CHECK(w[1].Target == 0 && w[1].Offset == 12 && w[1].NewValue == 1, "row1 locked inc");
		CHECK(w[2].Target == 2 && w[2].NewValue == (unsigned int)(uintptr_t)heapB && w[2].Frame == 2, "row2 base");
		CHECK(w[3].Target == 1 && w[3].OldValue == 99 && w[3].NewValue == 100, "row3 deref");
		CHECK(w[3].Candidates[0] == retIntoCaller, "row3 candidate %08X want %08X", w[3].Candidates[0], retIntoCaller);
		CHECK(w[4].Target == 2 && w[4].NewValue == (unsigned int)(uintptr_t)heapC, "row4 base");
		CHECK(w[5].Target == 1 && w[5].OldValue == 0 && w[5].NewValue == 7, "row5 deref after re-resolve");
		for (int i = 6; i < 10; ++i)
			CHECK(w[i].Target == 0 && w[i].NewValue == 0xEE, "rep row %d", i);
		CHECK(w[6].Offset == 16 && w[9].Offset == 28, "rep offsets %u..%u", w[6].Offset, w[9].Offset);
	}
	CHECK(NeighbourTraps() == 3, "neighbour %ld (want 3)", NeighbourTraps());
	CHECK(*(volatile unsigned int*)(heapB + 0x214) == 101, "released page write lost");
	Disarm();
	*(volatile unsigned int*)countries = 1;   // must not fault after disarm

	// Phase 2: a FULL watch table (the self-test borrows the slot past it) plus
	// exec targets.
	const unsigned int hooked = (unsigned int)(uintptr_t)&Hooked;
	const unsigned int point = (unsigned int)(uintptr_t)storepoint;
	unsigned char hookedBytes[8], pointBytes[5];
	memcpy(hookedBytes, (const void*)(uintptr_t)hooked, 8);
	memcpy(pointBytes, storepoint, 5);
	const unsigned int watched = dataAddr + 0x800;
	char spec2[512];
	int len = 0;
	for (int i = 0; i < MaxTargets - 1; ++i)
		len += snprintf(spec2 + len, sizeof spec2 - len, "%X:4,", dataAddr + 0x400 + 0x10 * i);
	snprintf(spec2 + len, sizeof spec2 - len, "%X:4, X:%X@8/esp+4/esp+8>+1C4/esp+4>0#2, x:0x%X@5/eax+0", watched, hooked, point);
	const bool ok2 = Arm(spec2, FrameNow, err, sizeof err);
	printf("arm2=%d err=\"%s\" selftest=%d xselftest=%d targets=%d exec=%d\n", ok2, err, SelfTestHits(), ExecSelfTestHits(), TargetCount(), ExecCount());
	CHECK(ok2, "arm2 failed: %s", err);
	CHECK(SelfTestHits() == 2, "full-table selftest %d", SelfTestHits());
	CHECK(ExecSelfTestHits() == 2, "exec selftest %d", ExecSelfTestHits());
	CHECK(TargetCount() == MaxTargets && ExecCount() == 2, "targets %d exec %d", TargetCount(), ExecCount());

	Obj obj = {};
	obj.vtable = 0x12345678;
	obj.field = 40;
	g_frame = 10;
	const int r1 = CallHooked(5, &obj);
	obj.field = 41;
	const int r2 = CallHooked(6, &obj);
	const int r3 = CallHooked(7, &obj);   // past #2: no row
	CHECK(r1 == 45 && r2 == 47 && r3 == 48, "hooked results %d %d %d", r1, r2, r3);
	CHECK(*(const unsigned char*)(uintptr_t)hooked == 0xE9, "no jmp at the hooked address while armed");
	g_frame = 11;
	StoreAtPoint((volatile unsigned int*)watched, 0x77);
	StoreAtPoint((volatile unsigned int*)watched, 0x78);

	CollectedX gx = {};
	DrainExec(CollectX, &gx);
	Collected gw = {};
	Drain(Collect, &gw);
	for (int i = 0; i < gx.n; ++i)
	{
		const ExecRow& x = gx.rows[i];
		printf("X=%d,t%u,esp=%08X,s0=%08X,s1=%08X,r=%08X/%08X/%08X ok=%X\n", x.Frame, x.Target, x.Regs[Esp], x.Stack[0], x.Stack[1], x.Reads[0], x.Reads[1], x.Reads[2], x.ReadOk);
	}
	for (int i = 0; i < gw.n; ++i)
		printf("W=%d,%08X,t%u,%u->%u\n", gw.rows[i].Frame, gw.rows[i].Eip, gw.rows[i].Target, gw.rows[i].OldValue, gw.rows[i].NewValue);
	CHECK(gx.n == 4, "exec rows %d (want 4)", gx.n);
	if (gx.n == 4)
	{
		const ExecRow* x = gx.rows;
		CHECK(x[0].Target == 0 && x[0].Frame == 10 && ReturnsFromCallTo(x[0].Stack[0], hooked) && x[0].Stack[1] == 5,
			"x0 s0 %08X is not a return from a call to Hooked", x[0].Stack[0]);
		CHECK(x[0].Reads[0] == 5 && x[0].Reads[1] == 40 && x[0].ReadOk == 3, "x0 reads %u %u ok %X", x[0].Reads[0], x[0].Reads[1], x[0].ReadOk);
		CHECK(x[1].Target == 0 && x[1].Reads[0] == 6 && x[1].Reads[1] == 41, "x1 reads");
		CHECK(x[2].Target == 1 && x[2].Frame == 11 && x[3].Target == 1, "store-point rows");
		CHECK(x[2].Regs[Eax] == watched && x[2].Regs[Ecx] == 0x77 && x[3].Regs[Ecx] == 0x78, "store-point regs");
		CHECK(x[2].Reads[0] == 0 && x[3].Reads[0] == 0x77, "store-point reads run before the store");
	}
	CHECK(gw.n == 2, "watch rows at the store point %d (want 2)", gw.n);
	if (gw.n == 2)
	{
		const unsigned int tramp = Exec(1).Trampoline;
		CHECK(gw.rows[0].Eip >= tramp && gw.rows[0].Eip < tramp + 64 && gw.rows[0].NewValue == 0x77, "w0 eip %08X", gw.rows[0].Eip);
		CHECK(gw.rows[1].OldValue == 0x77 && gw.rows[1].NewValue == 0x78, "w1");
	}
	Disarm();
	CHECK(memcmp((const void*)(uintptr_t)hooked, hookedBytes, 8) == 0, "Hooked's bytes not restored");
	CHECK(memcmp(storepoint, pointBytes, 5) == 0, "storepoint's bytes not restored");
	StoreAtPoint((volatile unsigned int*)watched, 1);   // must not trap after disarm
	CHECK(CallHooked(1, &obj) == 42, "Hooked after disarm");
	printf("VERDICT: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures;
}
