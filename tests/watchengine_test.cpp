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

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv)
{
	freopen("engtest.out", "w", stdout);
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
	CHECK(NeighbourTraps() == 2, "neighbour %ld (want 2)", NeighbourTraps());
	CHECK(*(volatile unsigned int*)(heapB + 0x214) == 101, "released page write lost");
	Disarm();
	*(volatile unsigned int*)countries = 1;   // must not fault after disarm
	printf("VERDICT: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures;
}
