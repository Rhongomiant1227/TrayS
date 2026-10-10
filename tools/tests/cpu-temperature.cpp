// Run TrayS's actual native DLL loading and CPU display path without starting
// its UI, watchdog, updater, or touching Explorer/user configuration.
#include "framework.h"
#include "TrayS.h"
#include <cstdio>
#include "TrayS.cpp"

int main()
{
    ConfigureSafeDllSearch();
    LoadTemperatureDLL();
    if (!hOHMA || !GetTemperature)
    {
        std::puts("CPU temperature is unavailable: PawnIO device or the monitor DLL is missing/inaccessible.");
        FreeTemperatureDLL();
        return 2;
    }

    int usableSamples = 0;
    for (int sample = 1; sample <= 5; ++sample)
    {
        const int temperature = GetCpuTemp(1);
        std::printf("TrayS native sample %d: CPU=%d C\n", sample, temperature);
        if (temperature > 0 && temperature <= 255)
            ++usableSamples;
        if (sample < 5)
            Sleep(1000);
    }
    FreeTemperatureDLL();
    if (usableSamples != 5)
    {
        std::puts("FAIL: TrayS did not return a usable CPU temperature for every sample.");
        return 1;
    }
    std::puts("PASS: TrayS's native loader and GetCpuTemp returned five usable CPU temperatures.");
    return 0;
}
