/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2018 Nest Labs, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *          Provides an implementation of the PlatformManager object
 *          for webOS platforms.
 */

#include <platform/internal/CHIPDeviceLayerInternal.h>

#include <app-common/zap-generated/enums.h>
#include <app-common/zap-generated/ids/Events.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/DeviceControlServer.h>
#include <platform/PlatformManager.h>
#include <platform/internal/GenericPlatformManagerImpl_POSIX.ipp>
#include <platform/webos/DeviceInfoProviderImpl.h>
#include <platform/webos/DiagnosticDataProviderImpl.h>

#include <thread>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>

#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
#define gettid() syscall(SYS_gettid)
#endif

using namespace ::chip::app::Clusters;

namespace chip {
namespace DeviceLayer {

PlatformManagerImpl PlatformManagerImpl::sInstance;

namespace {

void SignalHandler(int signum)
{
    ChipLogDetail(DeviceLayer, "Caught signal %d", signum);

    switch (signum)
    {
    case SIGUSR1:
        PlatformMgrImpl().HandleSoftwareFault(SoftwareDiagnostics::Events::SoftwareFault::Id);
        break;
    case SIGUSR2:
        PlatformMgrImpl().HandleGeneralFault(GeneralDiagnostics::Events::HardwareFaultChange::Id);
        break;
    case SIGHUP:
        PlatformMgrImpl().HandleGeneralFault(GeneralDiagnostics::Events::RadioFaultChange::Id);
        break;
    case SIGTTIN:
        PlatformMgrImpl().HandleGeneralFault(GeneralDiagnostics::Events::NetworkFaultChange::Id);
        break;
    case SIGTSTP:
        PlatformMgrImpl().HandleSwitchEvent(Switch::Events::SwitchLatched::Id);
        break;
    default:
        break;
    }
}

#if CHIP_WITH_GIO
void GDBus_Thread()
{
    GMainLoop * loop = g_main_loop_new(nullptr, false);

    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}
#endif
} // namespace

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
void PlatformManagerImpl::WiFIIPChangeListener()
{
    int sock;
    if ((sock = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) == -1)
    {
        ChipLogError(DeviceLayer, "Failed to init netlink socket for ip addresses.");
        return;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_IPV4_IFADDR;

    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) == -1)
    {
        ChipLogError(DeviceLayer, "Failed to bind netlink socket for ip addresses.");
        return;
    }

    ssize_t len;
    char buffer[4096];
    for (struct nlmsghdr * header = reinterpret_cast<struct nlmsghdr *>(buffer); (len = recv(sock, header, sizeof(buffer), 0)) > 0;)
    {
        for (struct nlmsghdr * messageHeader = header;
             (NLMSG_OK(messageHeader, static_cast<uint32_t>(len))) && (messageHeader->nlmsg_type != NLMSG_DONE);
             messageHeader = NLMSG_NEXT(messageHeader, len))
        {
            if (header->nlmsg_type == RTM_NEWADDR)
            {
                struct ifaddrmsg * addressMessage = (struct ifaddrmsg *) NLMSG_DATA(header);
                struct rtattr * routeInfo         = IFA_RTA(addressMessage);
                size_t rtl                        = IFA_PAYLOAD(header);

                for (; rtl && RTA_OK(routeInfo, rtl); routeInfo = RTA_NEXT(routeInfo, rtl))
                {
                    if (routeInfo->rta_type == IFA_LOCAL)
                    {
                        char name[IFNAMSIZ];
                        if (if_indextoname(addressMessage->ifa_index, name) == nullptr)
                        {
                            ChipLogError(DeviceLayer, "Error %d when getting the interface name at index: %d", errno,
                                         addressMessage->ifa_index);
                            continue;
                        }

                        if (strcmp(name, ConnectivityManagerImpl::GetWiFiIfName()) != 0)
                        {
                            continue;
                        }

                        char ipStrBuf[chip::Inet::IPAddress::kMaxStringLength] = { 0 };
                        inet_ntop(AF_INET, RTA_DATA(routeInfo), ipStrBuf, sizeof(ipStrBuf));
                        ChipLogDetail(DeviceLayer, "Got IP address on interface: %s IP: %s", name, ipStrBuf);

                        ChipDeviceEvent event;
                        event.Type                            = DeviceEventType::kInternetConnectivityChange;
                        event.InternetConnectivityChange.IPv4 = kConnectivity_Established;
                        event.InternetConnectivityChange.IPv6 = kConnectivity_NoChange;
                        VerifyOrDie(chip::Inet::IPAddress::FromString(ipStrBuf, event.InternetConnectivityChange.ipAddress));

                        CHIP_ERROR status = PlatformMgr().PostEvent(&event);
                        if (status != CHIP_NO_ERROR)
                        {
                            ChipLogDetail(DeviceLayer, "Failed to report IP address: %" CHIP_ERROR_FORMAT, status.Format());
                        }
                    }
                }
            }
        }
    }
}
#endif // #if CHIP_DEVICE_CONFIG_ENABLE_WIFI

CHIP_ERROR PlatformManagerImpl::_InitChipStack()
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SignalHandler;
    sigaction(SIGHUP, &action, nullptr);
    sigaction(SIGTTIN, &action, nullptr);
    sigaction(SIGUSR1, &action, nullptr);
    sigaction(SIGUSR2, &action, nullptr);
    sigaction(SIGTSTP, &action, nullptr);

#if CHIP_WITH_GIO
    GError * error = nullptr;

    this->mpGDBusConnection = UniqueGDBusConnection(g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error));

    std::thread gdbusThread(GDBus_Thread);
    gdbusThread.detach();
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
    std::thread wifiIPThread(WiFIIPChangeListener);
    wifiIPThread.detach();
#endif

    // Initialize the configuration system.
    ReturnErrorOnFailure(Internal::PosixConfig::Init());
    SetConfigurationMgr(&ConfigurationManagerImpl::GetDefaultInstance());
    SetDiagnosticDataProvider(&DiagnosticDataProviderImpl::GetDefaultInstance());
    SetDeviceInfoProvider(&DeviceInfoProviderImpl::GetDefaultInstance());

    // Call _InitChipStack() on the generic implementation base class
    // to finish the initialization process.
    ReturnErrorOnFailure(Internal::GenericPlatformManagerImpl_POSIX<PlatformManagerImpl>::_InitChipStack());

    mStartTime = System::SystemClock().GetMonotonicTimestamp();

    return CHIP_NO_ERROR;
}

CHIP_ERROR PlatformManagerImpl::_Shutdown()
{
    uint64_t upTime = 0;

    if (GetDiagnosticDataProvider().GetUpTime(upTime) == CHIP_NO_ERROR)
    {
        uint32_t totalOperationalHours = 0;

        if (ConfigurationMgr().GetTotalOperationalHours(totalOperationalHours) == CHIP_NO_ERROR)
        {
            ConfigurationMgr().StoreTotalOperationalHours(totalOperationalHours + static_cast<uint32_t>(upTime / 3600));
        }
        else
        {
            ChipLogError(DeviceLayer, "Failed to get total operational hours of the Node");
        }
    }
    else
    {
        ChipLogError(DeviceLayer, "Failed to get current uptime since the Node’s last reboot");
    }

    return Internal::GenericPlatformManagerImpl_POSIX<PlatformManagerImpl>::_Shutdown();
}

void PlatformManagerImpl::HandleGeneralFault(uint32_t EventId)
{
    GeneralDiagnosticsDelegate * delegate = GetDiagnosticDataProvider().GetGeneralDiagnosticsDelegate();

    if (delegate == nullptr)
    {
        ChipLogError(DeviceLayer, "No delegate registered to handle General Diagnostics event");
        return;
    }

    if (EventId == GeneralDiagnostics::Events::HardwareFaultChange::Id)
    {
        GeneralFaults<kMaxHardwareFaults> previous;
        GeneralFaults<kMaxHardwareFaults> current;

#if CHIP_CONFIG_TEST
        // On Linux Simulation, set following hardware faults statically.
        ReturnOnFailure(previous.add(EMBER_ZCL_HARDWARE_FAULT_TYPE_RADIO));
        ReturnOnFailure(previous.add(EMBER_ZCL_HARDWARE_FAULT_TYPE_POWER_SOURCE));

        ReturnOnFailure(current.add(EMBER_ZCL_HARDWARE_FAULT_TYPE_RADIO));
        ReturnOnFailure(current.add(EMBER_ZCL_HARDWARE_FAULT_TYPE_SENSOR));
        ReturnOnFailure(current.add(EMBER_ZCL_HARDWARE_FAULT_TYPE_POWER_SOURCE));
        ReturnOnFailure(current.add(EMBER_ZCL_HARDWARE_FAULT_TYPE_USER_INTERFACE_FAULT));
#endif
        delegate->OnHardwareFaultsDetected(previous, current);
    }
    else if (EventId == GeneralDiagnostics::Events::RadioFaultChange::Id)
    {
        GeneralFaults<kMaxRadioFaults> previous;
        GeneralFaults<kMaxRadioFaults> current;

#if CHIP_CONFIG_TEST
        // On Linux Simulation, set following radio faults statically.
        ReturnOnFailure(previous.add(EMBER_ZCL_RADIO_FAULT_TYPE_WI_FI_FAULT));
        ReturnOnFailure(previous.add(EMBER_ZCL_RADIO_FAULT_TYPE_THREAD_FAULT));

        ReturnOnFailure(current.add(EMBER_ZCL_RADIO_FAULT_TYPE_WI_FI_FAULT));
        ReturnOnFailure(current.add(EMBER_ZCL_RADIO_FAULT_TYPE_CELLULAR_FAULT));
        ReturnOnFailure(current.add(EMBER_ZCL_RADIO_FAULT_TYPE_THREAD_FAULT));
        ReturnOnFailure(current.add(EMBER_ZCL_RADIO_FAULT_TYPE_NFC_FAULT));
#endif
        delegate->OnRadioFaultsDetected(previous, current);
    }
    else if (EventId == GeneralDiagnostics::Events::NetworkFaultChange::Id)
    {
        GeneralFaults<kMaxNetworkFaults> previous;
        GeneralFaults<kMaxNetworkFaults> current;

#if CHIP_CONFIG_TEST
        // On Linux Simulation, set following radio faults statically.
        ReturnOnFailure(previous.add(EMBER_ZCL_NETWORK_FAULT_TYPE_HARDWARE_FAILURE));
        ReturnOnFailure(previous.add(EMBER_ZCL_NETWORK_FAULT_TYPE_NETWORK_JAMMED));

        ReturnOnFailure(current.add(EMBER_ZCL_NETWORK_FAULT_TYPE_HARDWARE_FAILURE));
        ReturnOnFailure(current.add(EMBER_ZCL_NETWORK_FAULT_TYPE_NETWORK_JAMMED));
        ReturnOnFailure(current.add(EMBER_ZCL_NETWORK_FAULT_TYPE_CONNECTION_FAILED));
#endif
        delegate->OnNetworkFaultsDetected(previous, current);
    }
    else
    {
        ChipLogError(DeviceLayer, "Unknow event ID:%d", EventId);
    }
}

void PlatformManagerImpl::HandleSoftwareFault(uint32_t EventId)
{
    SoftwareDiagnosticsDelegate * delegate = GetDiagnosticDataProvider().GetSoftwareDiagnosticsDelegate();

    if (delegate != nullptr)
    {
        SoftwareDiagnostics::Structs::SoftwareFaultStruct::Type softwareFault;
        char threadName[kMaxThreadNameLength + 1];

        softwareFault.id = gettid();
        strncpy(threadName, std::to_string(softwareFault.id).c_str(), kMaxThreadNameLength);
        threadName[kMaxThreadNameLength] = '\0';
        softwareFault.name               = CharSpan::fromCharString(threadName);
        softwareFault.faultRecording     = ByteSpan(Uint8::from_const_char("FaultRecording"), strlen("FaultRecording"));

        delegate->OnSoftwareFaultDetected(softwareFault);
    }
}

void PlatformManagerImpl::HandleSwitchEvent(uint32_t EventId)
{
    SwitchDeviceControlDelegate * delegate = DeviceControlServer::DeviceControlSvr().GetSwitchDelegate();

    if (delegate == nullptr)
    {
        ChipLogError(DeviceLayer, "No delegate registered to handle Switch event");
        return;
    }

    if (EventId == Switch::Events::SwitchLatched::Id)
    {
        uint8_t newPosition = 0;

#if CHIP_CONFIG_TEST
        newPosition = 100;
#endif
        delegate->OnSwitchLatched(newPosition);
    }
    else if (EventId == Switch::Events::InitialPress::Id)
    {
        uint8_t newPosition = 0;

#if CHIP_CONFIG_TEST
        newPosition = 100;
#endif
        delegate->OnInitialPressed(newPosition);
    }
    else if (EventId == Switch::Events::LongPress::Id)
    {
        uint8_t newPosition = 0;

#if CHIP_CONFIG_TEST
        newPosition = 100;
#endif
        delegate->OnLongPressed(newPosition);
    }
    else if (EventId == Switch::Events::ShortRelease::Id)
    {
        uint8_t previousPosition = 0;

#if CHIP_CONFIG_TEST
        previousPosition = 50;
#endif
        delegate->OnShortReleased(previousPosition);
    }
    else if (EventId == Switch::Events::LongRelease::Id)
    {
        uint8_t previousPosition = 0;

#if CHIP_CONFIG_TEST
        previousPosition = 50;
#endif
        delegate->OnLongReleased(previousPosition);
    }
    else if (EventId == Switch::Events::MultiPressOngoing::Id)
    {
        uint8_t newPosition                   = 0;
        uint8_t currentNumberOfPressesCounted = 0;

#if CHIP_CONFIG_TEST
        newPosition                   = 10;
        currentNumberOfPressesCounted = 5;
#endif
        delegate->OnMultiPressOngoing(newPosition, currentNumberOfPressesCounted);
    }
    else if (EventId == Switch::Events::MultiPressComplete::Id)
    {
        uint8_t newPosition                 = 0;
        uint8_t totalNumberOfPressesCounted = 0;

#if CHIP_CONFIG_TEST
        newPosition                 = 10;
        totalNumberOfPressesCounted = 5;
#endif
        delegate->OnMultiPressComplete(newPosition, totalNumberOfPressesCounted);
    }
    else
    {
        ChipLogError(DeviceLayer, "Unknow event ID:%d", EventId);
    }
}

#if CHIP_WITH_GIO
GDBusConnection * PlatformManagerImpl::GetGDBusConnection()
{
    return this->mpGDBusConnection.get();
}
#endif

} // namespace DeviceLayer
} // namespace chip
