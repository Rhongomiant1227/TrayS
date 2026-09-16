#pragma once
#include <memory>
#include "OpenHardwareMonitorGlobal.h"
#include <map>
#include <string>
#include <iterator>
#include <cmath>
#include <mutex>

namespace OpenHardwareMonitorApi
{
    class IOpenHardwareMonitor
    {
    public:
        virtual ~IOpenHardwareMonitor() = default;
        virtual void GetHardwareInfo() = 0;     //获取一次硬件信息
        virtual float CpuTemperature() = 0;     //返回获取到的CPU温度
        virtual float GpuTemperature() = 0;     //返回获取到的GPU温度
        virtual float HDDTemperature() = 0;     //返回获取到的硬盘温度
        virtual float MainboardTemperature() = 0;   //返回获取到的主板温度
        virtual float GpuUsage() = 0;           //返回获取到的GPU利用率
        virtual const std::map<std::wstring, float>& AllHDDTemperature() = 0;   //返回所有硬盘的温度。map的key是硬盘的名称，value是硬盘的温度
        virtual const std::map<std::wstring, float>& AllCpuTemperature() = 0;   //返回所有CPU（核心）的温度。map的key是CPU的名称，value是硬盘的温度
        virtual const std::map<std::wstring, float>& AllHDDUsage() = 0;         //返回所有硬盘的使用率

        virtual void SetCpuEnable(bool enable) = 0;
        virtual void SetGpuEnable(bool enable) = 0;
        virtual void SetHddEnable(bool enable) = 0;
        virtual void SetMainboardEnable(bool enable) = 0;
    };

    std::shared_ptr<IOpenHardwareMonitor> CreateInstance();
//    OPENHARDWAREMONITOR_API std::wstring GetErrorMessage();
}
std::shared_ptr<OpenHardwareMonitorApi::IOpenHardwareMonitor> m_pMonitor{};
extern "C" OPENHARDWAREMONITOR_API void GetTemperature(float* fCpu,float * fGpu,float* fMain,float *fHdd,int iHDD,float * fCpuPackge)
{
    // The C ABI is called from the UI worker thread. Always initialize optional
    // outputs so a missing sensor cannot leak stale stack/heap values to TrayS.
    if (fCpu) *fCpu = -1.0f;
    if (fGpu) *fGpu = -1.0f;
    if (fMain) *fMain = -1.0f;
    if (fHdd) *fHdd = -1.0f;
    if (fCpuPackge) *fCpuPackge = -1.0f;

    try
    {
        // TrayS normally serializes calls with its temperature SRW lock, but
        // this exported entry point is also a public C ABI. Serialize the
        // monitor singleton here so a future caller cannot race initialization,
        // managed traversal, map reads, or DLL teardown.
        static std::mutex monitor_mutex;
        std::lock_guard<std::mutex> monitor_lock(monitor_mutex);
        if (m_pMonitor == 0)
        {
            m_pMonitor = OpenHardwareMonitorApi::CreateInstance();
            if (m_pMonitor == 0)
                return;
            if (fCpu || fCpuPackge)
                m_pMonitor->SetCpuEnable(true);
            if (fGpu)
                m_pMonitor->SetGpuEnable(true);
            if (fHdd)
                m_pMonitor->SetHddEnable(true);
            if (fMain)
                m_pMonitor->SetMainboardEnable(true);
        }
        float cpu = -1.0f;
        float gpu = -1.0f;
        float mainboard = -1.0f;
        float hdd = -1.0f;
        float cpuPackage = -1.0f;

        m_pMonitor->GetHardwareInfo();

        if (fCpu || fCpuPackge)
        {
            const auto& temperatures = m_pMonitor->AllCpuTemperature();
            auto iter = temperatures.find(L"CPU Core #1");
            if (iter == temperatures.end() && !temperatures.empty())
                iter = temperatures.begin();
            if (iter != temperatures.end())
                cpu = iter->second;

            iter = temperatures.find(L"CPU Package");
            if (iter == temperatures.end() && !temperatures.empty())
                iter = temperatures.begin();
            if (iter != temperatures.end())
                cpuPackage = iter->second;
        }

        if (fGpu)
            gpu = m_pMonitor->GpuTemperature();
        if (fMain)
            mainboard = m_pMonitor->MainboardTemperature();
        if (fHdd)
        {
            const auto& temperatures = m_pMonitor->AllHDDTemperature();
            if (iHDD == -1)
            {
                for (const auto& item : temperatures)
                {
                    if (item.second > hdd)
                        hdd = item.second;
                }
            }
            else if (iHDD >= 0 && static_cast<size_t>(iHDD) < temperatures.size())
            {
                auto iter = temperatures.begin();
                std::advance(iter, static_cast<size_t>(iHDD));
                hdd = iter->second;
            }
        }

        if (!std::isfinite(cpu) || cpu < -50.0f || cpu > 255.0f)
            cpu = -1.0f;
        if (!std::isfinite(gpu) || gpu < -50.0f || gpu > 255.0f)
            gpu = -1.0f;
        if (!std::isfinite(mainboard) || mainboard < -50.0f || mainboard > 255.0f)
            mainboard = -1.0f;
        if (!std::isfinite(hdd) || hdd < -50.0f || hdd > 255.0f)
            hdd = -1.0f;
        if (!std::isfinite(cpuPackage) || cpuPackage < -50.0f || cpuPackage > 255.0f)
            cpuPackage = -1.0f;

        if (fCpu) *fCpu = cpu;
        if (fGpu) *fGpu = gpu;
        if (fMain) *fMain = mainboard;
        if (fHdd) *fHdd = hdd;
        if (fCpuPackge) *fCpuPackge = cpuPackage;
    }
    catch (...)
    {
        // Keep all outputs at their initialized sentinel values.  No managed
        // exception is allowed to cross this C ABI boundary.
    }
}
