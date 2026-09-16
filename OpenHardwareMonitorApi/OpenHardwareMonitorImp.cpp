// 这是主 DLL 文件。

#include "stdafx.h"

#include "OpenHardwareMonitorImp.h"
#include <cmath>
#include <utility>
#include <vector>

namespace OpenHardwareMonitorApi
{
    static std::wstring error_message;

    //将CRL的String类型转换成C++的std::wstring类型
    static std::wstring ClrStringToStdWstring(System::String^ str)
    {
        if (str == nullptr)
        {
            return std::wstring();
        }
        else
        {
            const wchar_t* chars = (const wchar_t*)(Runtime::InteropServices::Marshal::StringToHGlobalUni(str)).ToPointer();
            std::wstring os = chars;
            Runtime::InteropServices::Marshal::FreeHGlobal(IntPtr((void*)chars));
            return os;
        }
    }


    std::shared_ptr<IOpenHardwareMonitor> CreateInstance()
    {
        std::shared_ptr<IOpenHardwareMonitor> pMonitor;
        try
        {
            MonitorGlobal::Instance()->Init();
            pMonitor = std::make_shared<COpenHardwareMonitor>();
        }
        catch (System::Exception^ e)
        {
            error_message = ClrStringToStdWstring(e->Message);
        }
        catch (...)
        {
            error_message = L"Unknown hardware monitor initialization error";
        }
        return pMonitor;
    }

    std::wstring GetErrorMessage()
    {
        return error_message;
    }

    float COpenHardwareMonitor::CpuTemperature()
    {
        return m_cpu_temperature;
    }

    float COpenHardwareMonitor::GpuTemperature()
    {
        if (m_gpu_nvidia_temperature >= 0)
            return m_gpu_nvidia_temperature;
        else
            return m_gpu_ati_temperature;
    }

    float COpenHardwareMonitor::HDDTemperature()
    {
        return m_hdd_temperature;
    }

    float COpenHardwareMonitor::MainboardTemperature()
    {
        return m_main_board_temperature;
    }

    float COpenHardwareMonitor::GpuUsage()
    {
        if (m_gpu_nvidia_usage >= 0)
            return m_gpu_nvidia_usage;
        else
            return m_gpu_ati_usage;
    }

    const std::map<std::wstring, float>& COpenHardwareMonitor::AllHDDTemperature()
    {
        return m_all_hdd_temperature;
    }

    const std::map<std::wstring, float>& COpenHardwareMonitor::AllCpuTemperature()
    {
        return m_all_cpu_temperature;
    }

    const std::map<std::wstring, float>& COpenHardwareMonitor::AllHDDUsage()
    {
        return m_all_hdd_usage;
    }

    void COpenHardwareMonitor::SetCpuEnable(bool enable)
    {
        auto global = MonitorGlobal::Instance();
        if (global != nullptr && global->computer != nullptr)
            global->computer->IsCpuEnabled = enable;
    }

    void COpenHardwareMonitor::SetGpuEnable(bool enable)
    {
        auto global = MonitorGlobal::Instance();
        if (global != nullptr && global->computer != nullptr)
            global->computer->IsGpuEnabled = enable;
    }

    void COpenHardwareMonitor::SetHddEnable(bool enable)
    {
        auto global = MonitorGlobal::Instance();
        if (global != nullptr && global->computer != nullptr)
            global->computer->IsStorageEnabled = enable;
    }

    void COpenHardwareMonitor::SetMainboardEnable(bool enable)
    {
        auto global = MonitorGlobal::Instance();
        if (global != nullptr && global->computer != nullptr)
            global->computer->IsMotherboardEnabled = enable;
    }

    static bool TryGetSensorValue(ISensor^ sensor, float& value)
    {
		value = -1.0f;
        if (sensor == nullptr)
            return false;
        try
        {
            Nullable<float> sensor_value = sensor->Value;
            if (!sensor_value.HasValue)
                return false;

            value = sensor_value.Value;
			// LHM reports Celsius/percentage values.  Reject provider sentinels
			// and physically impossible values before they enter the aggregate
			// maps or the unmanaged UI's integer formatting paths.
			return std::isfinite(value) && value >= -50.0f && value <= 255.0f;
        }
        catch (...)
        {
            return false;
        }
    }

    static bool TryGetLoadValue(ISensor^ sensor, float& value)
    {
        if (!TryGetSensorValue(sensor, value))
            return false;
        if (value < 0.0f || value > 100.0f)
        {
            value = -1.0f;
            return false;
        }
        return true;
    }

    bool COpenHardwareMonitor::GetHardwareTemperature(IHardware^ hardware, float& temperature)
    {
        temperature = -1;
        if (hardware == nullptr)
            return false;

        std::vector<float> all_temperature;
        float core_temperature{ -1 };
        bool has_core_temperature = false;
        System::String^ temperature_name;
        switch (hardware->HardwareType)
        {
        case HardwareType::Cpu:
            temperature_name = L"Core Average";
            break;
        case HardwareType::GpuNvidia: case HardwareType::GpuAmd:
            temperature_name = L"GPU Core";
            break;
        default:
            break;
        }
        if (hardware->Sensors != nullptr)
        {
            for (int i = 0; i < hardware->Sensors->Length; i++)
            {
                //找到温度传感器
                if (hardware->Sensors[i] != nullptr && hardware->Sensors[i]->SensorType == SensorType::Temperature)
                {
                    float cur_temperture = -1;
                    if (!TryGetSensorValue(hardware->Sensors[i], cur_temperture))
                        continue;
                    all_temperature.push_back(cur_temperture);
                    if (hardware->Sensors[i]->Name == temperature_name) //如果找到了名称为temperature_name的温度传感器，则将温度保存到core_temperature里
                    {
                        core_temperature = cur_temperture;
                        has_core_temperature = true;
                    }
                }
            }
        }
        if (has_core_temperature)
        {
            if (std::isfinite(core_temperature))
            {
                temperature = core_temperature;
                return true;
            }
            return false;
        }
        if (!all_temperature.empty())
        {
            //如果有多个温度传感器，则取平均值
            double sum{};
            for (auto i : all_temperature)
                sum += i;
            temperature = sum / all_temperature.size();
            if (std::isfinite(sum) && std::isfinite(temperature) && temperature >= -50.0f && temperature <= 255.0f)
                return true;
            temperature = -1.0f;
            return false;
        }
        //如果没有找到温度传感器，则在SubHardware中寻找
        if (hardware->SubHardware == nullptr)
            return false;
        for (int i = 0; i < hardware->SubHardware->Length; i++)
        {
            if (hardware->SubHardware[i] != nullptr && GetHardwareTemperature(hardware->SubHardware[i], temperature))
                return true;
        }
        return false;
    }

    bool COpenHardwareMonitor::GetCpuTemperature(IHardware^ hardware, float& temperature)
    {
		temperature = -1.0f;
        if (hardware == nullptr || hardware->Sensors == nullptr)
        {
			if (hardware != nullptr && hardware->SubHardware != nullptr)
			{
				for (int i = 0; i < hardware->SubHardware->Length; ++i)
				{
					if (hardware->SubHardware[i] != nullptr && GetCpuTemperature(hardware->SubHardware[i], temperature))
						return true;
				}
			}
			return false;
		}
        for (int i = 0; i < hardware->Sensors->Length; i++)
        {
            //找到温度传感器
            if (hardware->Sensors[i] != nullptr && hardware->Sensors[i]->SensorType == SensorType::Temperature)
            {
                String^ name = hardware->Sensors[i]->Name;
                float sensor_temperature = -1;
                if (!TryGetSensorValue(hardware->Sensors[i], sensor_temperature))
                    continue;
                //保存每个CPU传感器的温度
                InsertValueToMap(m_all_cpu_temperature, ClrStringToStdWstring(name), sensor_temperature);
            }
        }
        //计算平均温度
        if (!m_all_cpu_temperature.empty())
        {
            double sum{};
            for (const auto& item : m_all_cpu_temperature)
                sum += item.second;
            temperature = sum / m_all_cpu_temperature.size();
            if (!std::isfinite(sum) || !std::isfinite(temperature) || temperature < -50.0f || temperature > 255.0f)
                temperature = -1.0f;
        }
        if (m_all_cpu_temperature.empty() && hardware->SubHardware != nullptr)
		{
			for (int i = 0; i < hardware->SubHardware->Length; ++i)
			{
				if (hardware->SubHardware[i] != nullptr && GetCpuTemperature(hardware->SubHardware[i], temperature))
					return true;
			}
		}
		return !m_all_cpu_temperature.empty() && std::isfinite(temperature);
    }

    bool COpenHardwareMonitor::GetGpuUsage(IHardware^ hardware, float& gpu_usage)
    {
		gpu_usage = -1.0f;
        if (hardware == nullptr || hardware->Sensors == nullptr)
            return false;
        for (int i = 0; i < hardware->Sensors->Length; i++)
        {
            //找到负载
            if (hardware->Sensors[i] != nullptr && hardware->Sensors[i]->SensorType == SensorType::Load)
            {
                if (hardware->Sensors[i]->Name == L"GPU Core")
                {
                    return TryGetLoadValue(hardware->Sensors[i], gpu_usage);
                }
            }
        }
        return false;
    }

    bool COpenHardwareMonitor::GetHddUsage(IHardware^ hardware, float& hdd_usage)
    {
		hdd_usage = -1.0f;
        if (hardware == nullptr || hardware->Sensors == nullptr)
            return false;
        for (int i = 0; i < hardware->Sensors->Length; i++)
        {
            //找到负载
            if (hardware->Sensors[i] != nullptr && hardware->Sensors[i]->SensorType == SensorType::Load)
            {
                if (hardware->Sensors[i]->Name == L"Total Activity")
                {
                    return TryGetLoadValue(hardware->Sensors[i], hdd_usage);
                }
            }
        }
        return false;
    }

    COpenHardwareMonitor::COpenHardwareMonitor()
    {
        ResetAllValues();
    }

    COpenHardwareMonitor::~COpenHardwareMonitor()
    {
        MonitorGlobal::Instance()->UnInit();
    }

    void COpenHardwareMonitor::ResetAllValues()
    {
        m_cpu_temperature = -1;
        m_gpu_nvidia_temperature = -1;
        m_gpu_ati_temperature = -1;
        m_hdd_temperature = -1;
        m_main_board_temperature = -1;
        m_gpu_nvidia_usage = -1;
        m_gpu_ati_usage = -1;
        m_all_hdd_temperature.clear();
        m_all_cpu_temperature.clear();
        m_all_hdd_usage.clear();
    }

    void COpenHardwareMonitor::InsertValueToMap(std::map<std::wstring, float>& value_map, const std::wstring& key, float value)
    {
        std::wstring candidate = key;
        unsigned int suffix = 1;
        while (value_map.find(candidate) != value_map.end())
        {
            candidate = key + L" #" + std::to_wstring(suffix++);
        }
        value_map.emplace(std::move(candidate), value);
    }

    void COpenHardwareMonitor::GetHardwareInfo()
    {
        ResetAllValues();
        error_message.clear();
        try
        {
            wchar_t nDisk=L'0';
            auto computer = MonitorGlobal::Instance()->computer;
            if (computer == nullptr || MonitorGlobal::Instance()->updateVisitor == nullptr || computer->Hardware == nullptr)
            {
                error_message = L"Hardware monitor is not initialized";
                return;
            }
            computer->Accept(MonitorGlobal::Instance()->updateVisitor);
            for (int i = 0; i < computer->Hardware->Count; i++)
            {
                try
                {
                    IHardware^ hardware = computer->Hardware[i];
                    if (hardware == nullptr)
                        continue;
                    //查找硬件类型
                    switch (hardware->HardwareType)
                    {
                    case HardwareType::Cpu:
                        if (m_cpu_temperature < 0)
                            GetCpuTemperature(hardware, m_cpu_temperature);
                        break;
                    case HardwareType::GpuNvidia:
                        if (m_gpu_nvidia_temperature < 0)
                            GetHardwareTemperature(hardware, m_gpu_nvidia_temperature);
                        if (m_gpu_nvidia_usage < 0)
                            GetGpuUsage(hardware, m_gpu_nvidia_usage);
                        break;
                    case HardwareType::GpuAmd:
                        if (m_gpu_ati_temperature < 0)
                            GetHardwareTemperature(hardware, m_gpu_ati_temperature);
                        if (m_gpu_ati_usage < 0)
                            GetGpuUsage(hardware, m_gpu_ati_usage);
                        break;
                    case HardwareType::Storage:
                    {
                        float cur_hdd_temperature = -1;
                        GetHardwareTemperature(hardware, cur_hdd_temperature);
                        //m_all_hdd_temperature[ClrStringToStdWstring(hardware->Name)] = cur_hdd_temperature;
                        InsertValueToMap(m_all_hdd_temperature,nDisk + ClrStringToStdWstring(hardware->Name), cur_hdd_temperature);
                        float cur_hdd_usage = -1;
                        GetHddUsage(hardware, cur_hdd_usage);
                        //m_all_hdd_usage[ClrStringToStdWstring(hardware->Name)] = cur_hdd_usage;
                        InsertValueToMap(m_all_hdd_usage,nDisk+ ClrStringToStdWstring(hardware->Name), cur_hdd_usage);
						// A storage controller can expose an unavailable (-1) sensor before
						// another disk reports a valid value.  Do not let that first failure
						// suppress all later HDD temperatures.
						if (cur_hdd_temperature >= 0 &&
							(m_hdd_temperature < 0 || cur_hdd_temperature > m_hdd_temperature))
							m_hdd_temperature = cur_hdd_temperature;
                        ++nDisk;
                    }
                    break;
                    case HardwareType::Motherboard:
                        if (m_main_board_temperature < 0)
                        {
                            float fMain = -1.0f;
                            if (GetHardwareTemperature(hardware, fMain) && std::isfinite(fMain) && fMain >= -50.0f && fMain <= 255.0f && fMain > m_main_board_temperature)
                                m_main_board_temperature = fMain;
                        }
                        break;
                    default:
                        break;
                    }
                }
                catch (System::Exception^ e)
                {
                    error_message = ClrStringToStdWstring(e->Message);
                }
                catch (...)
                {
                    error_message = L"Unknown hardware node update error";
                }
            }
        }
        catch (System::Exception^ e)
        {
            error_message = ClrStringToStdWstring(e->Message);
        }
        catch (...)
        {
            error_message = L"Unknown hardware monitor update error";
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////
    MonitorGlobal::MonitorGlobal()
    {

    }

    MonitorGlobal::~MonitorGlobal()
    {

    }

    void MonitorGlobal::Init()
    {
        if (computer != nullptr)
            return;
        updateVisitor = gcnew UpdateVisitor();
        computer = gcnew Computer();
        try
        {
            computer->Open();
        }
        catch (...)
        {
			// Open() can fail after allocating managed monitor resources (and,
			// depending on the selected backend, after opening a driver handle).
			// Best-effort Close() keeps a partial initialization from surviving
			// until process teardown or DLL unload.
			try
			{
				if (computer != nullptr)
					computer->Close();
			}
			catch (...)
			{
			}
            computer = nullptr;
            updateVisitor = nullptr;
            throw;
        }
    }

    void MonitorGlobal::UnInit()
    {
        if (computer != nullptr)
        {
            try
            {
                computer->Close();
            }
            catch (...)
            {
                // Closing a partially initialized monitor must not escape a
                // DLL unload/destructor path.
            }
            computer = nullptr;
        }
        updateVisitor = nullptr;
    }

}
