#pragma once

namespace lavalign
{
    // Asks the OS not to throttle this process (Windows: no EcoQoS / efficiency-core demotion). No-op elsewhere.
    void requestFullSpeed();
}
