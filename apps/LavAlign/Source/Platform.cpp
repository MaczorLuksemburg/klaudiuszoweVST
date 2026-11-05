#include "Platform.h"

#ifdef _WIN32
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace lavalign
{
void requestFullSpeed()
{
#ifdef _WIN32
    // Windows 11 moves processes it considers background work (EcoQoS) onto efficiency cores and lower
    // clocks, which made long analyses several times slower once the window lost focus.
    PROCESS_POWER_THROTTLING_STATE state {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;  // opt out
    SetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof (state));
#endif
}
}
