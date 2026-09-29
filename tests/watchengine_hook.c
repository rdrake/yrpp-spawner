// One Syringe hook, so Syringe has something to place and reaches its --detach path.
#include <windows.h>
typedef struct { unsigned int addr; unsigned int size; const char *name; } hookdecl;
__declspec(dllexport) DWORD __cdecl probe_hook(void *regs) { (void)regs; SetEnvironmentVariableA("HOOK_RAN", "1"); return 0; }
__attribute__((section(".syhks00"), used)) hookdecl hk_probe = { HOOKADDR, 5, "probe_hook" };
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { (void)h; (void)r; (void)p; return TRUE; }
