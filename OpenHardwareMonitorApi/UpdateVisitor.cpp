#include "stdafx.h"
#include "UpdateVisitor.h"

namespace OpenHardwareMonitorApi
{
    void UpdateVisitor::VisitComputer(IComputer ^ computer)
    {
        if (computer == nullptr)
            return;
        try
        {
            computer->Traverse(this);
        }
        catch (...)
        {
            // A provider can fail while enumerating a single device.  Keep
            // that failure inside the managed traversal boundary so the
            // native caller receives a normal unavailable-sensor result.
        }
    }

    void UpdateVisitor::VisitHardware(IHardware ^ hardware)
    {
        if (hardware == nullptr)
            return;
        try
        {
            hardware->Update();
        }
        catch (...)
        {
            // A single unavailable sensor (for example an AMD firmware node
            // during a driver transition) must not abort traversal of all
            // remaining hardware.
        }
        try
        {
            auto subHardwareList = hardware->SubHardware;
            if (subHardwareList == nullptr)
                return;
            for each (IHardware^ subHardware in subHardwareList)
            {
                if (subHardware == nullptr)
                    continue;
                try
                {
                    subHardware->Accept(this);
                }
                catch (...)
                {
                    // Isolate a faulty child node (common during hot-plug or
                    // firmware transitions) from its siblings.
                }
            }
        }
        catch (...)
        {
            // Reading SubHardware or enumerating the managed collection can
            // itself fail; treat the collection as temporarily unavailable.
        }
    }

    void UpdateVisitor::VisitSensor(ISensor ^ sensor)
    {
    }

    void UpdateVisitor::VisitParameter(IParameter ^ parameter)
    {
    }

}
