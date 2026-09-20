#include "AEConfig.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEGP },
		Name { "QCBridgeAE" },
		Category { "General Plugin" },
		Version { 65536 },          /* 1.0.0 */
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EntryPointFunc"},
    #elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EntryPointFunc"},
    #endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EntryPointFunc"},
		CodeMacARM64 {"EntryPointFunc"},
#endif
	}
};
