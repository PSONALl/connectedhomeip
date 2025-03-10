/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
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

#pragma once

#include <app/AppBuildConfig.h>

#include <access/AccessControl.h>
#include <access/examples/ExampleAccessControlDelegate.h>
#include <app/CASEClientPool.h>
#include <app/CASESessionManager.h>
#include <app/DefaultAttributePersistenceProvider.h>
#include <app/OperationalDeviceProxyPool.h>
#include <app/TestEventTriggerDelegate.h>
#include <app/server/AclStorage.h>
#include <app/server/AppDelegate.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/DefaultAclStorage.h>
#include <credentials/FabricTable.h>
#include <credentials/GroupDataProvider.h>
#include <credentials/GroupDataProviderImpl.h>
#include <inet/InetConfig.h>
#include <lib/core/CHIPConfig.h>
#include <lib/support/SafeInt.h>
#include <messaging/ExchangeMgr.h>
#include <platform/KeyValueStoreManager.h>
#include <platform/KvsPersistentStorageDelegate.h>
#include <protocols/secure_channel/CASEServer.h>
#include <protocols/secure_channel/MessageCounterManager.h>
#include <protocols/secure_channel/PASESession.h>
#include <protocols/secure_channel/RendezvousParameters.h>
#if CHIP_CONFIG_ENABLE_SESSION_RESUMPTION
#include <protocols/secure_channel/SimpleSessionResumptionStorage.h>
#endif
#include <protocols/user_directed_commissioning/UserDirectedCommissioning.h>
#include <transport/SessionManager.h>
#include <transport/TransportMgr.h>
#include <transport/TransportMgrBase.h>
#if CONFIG_NETWORK_LAYER_BLE
#include <transport/raw/BLE.h>
#endif
#include <transport/raw/UDP.h>

namespace chip {

constexpr size_t kMaxBlePendingPackets = 1;

using ServerTransportMgr = chip::TransportMgr<chip::Transport::UDP
#if INET_CONFIG_ENABLE_IPV4
                                              ,
                                              chip::Transport::UDP
#endif
#if CONFIG_NETWORK_LAYER_BLE
                                              ,
                                              chip::Transport::BLE<kMaxBlePendingPackets>
#endif
                                              >;

struct ServerInitParams
{
    ServerInitParams()          = default;
    virtual ~ServerInitParams() = default;

    // Not copyable
    ServerInitParams(const ServerInitParams &) = delete;
    ServerInitParams & operator=(const ServerInitParams &) = delete;

    // Application delegate to handle some commissioning lifecycle events
    AppDelegate * appDelegate = nullptr;
    // Port to use for Matter commissioning/operational traffic
    uint16_t operationalServicePort = CHIP_PORT;
    // Port to use for UDC if supported
    uint16_t userDirectedCommissioningPort = CHIP_UDC_PORT;
    // Interface on which to run daemon
    Inet::InterfaceId interfaceId = Inet::InterfaceId::Null();

    // Persistent storage delegate: MUST be injected. Used to maintain storage by much common code.
    // Must be initialized before being provided.
    PersistentStorageDelegate * persistentStorageDelegate = nullptr;
    // Session resumption storage: Optional. Support session resumption when provided.
    // Must be initialized before being provided.
    SessionResumptionStorage * sessionResumptionStorage = nullptr;
    // Group data provider: MUST be injected. Used to maintain critical keys such as the Identity
    // Protection Key (IPK) for CASE. Must be initialized before being provided.
    Credentials::GroupDataProvider * groupDataProvider = nullptr;
    // Access control delegate: MUST be injected. Used to look up access control rules. Must be
    // initialized before being provided.
    Access::AccessControl::Delegate * accessDelegate = nullptr;
    // ACL storage: MUST be injected. Used to store ACL entries in persistent storage. Must NOT
    // be initialized before being provided.
    app::AclStorage * aclStorage = nullptr;
    // Network native params can be injected depending on the
    // selected Endpoint implementation
    void * endpointNativeParams = nullptr;
    // Optional. Support test event triggers when provided. Must be initialized before being
    // provided.
    TestEventTriggerDelegate * testEventTriggerDelegate = nullptr;
};

/**
 * Transitional version of ServerInitParams to assist SDK integrators in
 * transitioning to injecting product/platform-owned resources. This version
 * of `ServerInitParams` statically owns and initializes (via the
 * `InitializeStaticResourcesBeforeServerInit()` method) the persistent storage
 * delegate, the group data provider, and the access control delegate. This is to reduce
 * the amount of copied boilerplate in all the example initializations (e.g. AppTask.cpp,
 * main.cpp).
 *
 * This version SHOULD BE USED ONLY FOR THE IN-TREE EXAMPLES.
 *
 * ACTION ITEMS FOR TRANSITION from a example in-tree to a product:
 *
 * While this could be used indefinitely, it does not exemplify orderly management of
 * application-injected resources. It is recommended for actual products to instead:
 *   - Use the basic ServerInitParams in the application
 *   - Have the application own an instance of the resources being injected in its own
 *     state (e.g. an implementation of PersistentStorageDelegate and GroupDataProvider
 *     interfaces).
 *   - Initialize the injected resources prior to calling Server::Init()
 *   - De-initialize the injected resources after calling Server::Shutdown()
 *
 * WARNING: DO NOT replicate the pattern shown here of having a subclass of ServerInitParams
 *          own the resources outside of examples. This was done to reduce the amount of change
 *          to existing examples while still supporting non-example versions of the
 *          resources to be injected.
 */
struct CommonCaseDeviceServerInitParams : public ServerInitParams
{
    CommonCaseDeviceServerInitParams() = default;

    // Not copyable
    CommonCaseDeviceServerInitParams(const CommonCaseDeviceServerInitParams &) = delete;
    CommonCaseDeviceServerInitParams & operator=(const CommonCaseDeviceServerInitParams &) = delete;

    /**
     * Call this before Server::Init() to initialize the internally-owned resources.
     * Server::Init() will fail if this is not done, since several params required to
     * be non-null will be null without calling this method. ** See the transition method
     * in the outer comment of this class **.
     *
     * @return CHIP_NO_ERROR on success or a CHIP_ERROR value from APIs called to initialize
     *         resources on failure.
     */
    virtual CHIP_ERROR InitializeStaticResourcesBeforeServerInit()
    {
        static chip::KvsPersistentStorageDelegate sKvsPersistenStorageDelegate;
        static chip::Credentials::GroupDataProviderImpl sGroupDataProvider;
#if CHIP_CONFIG_ENABLE_SESSION_RESUMPTION
        static chip::SimpleSessionResumptionStorage sSessionResumptionStorage;
#endif
        static chip::app::DefaultAclStorage sAclStorage;

        // KVS-based persistent storage delegate injection
        chip::DeviceLayer::PersistedStorage::KeyValueStoreManager & kvsManager = DeviceLayer::PersistedStorage::KeyValueStoreMgr();
        ReturnErrorOnFailure(sKvsPersistenStorageDelegate.Init(&kvsManager));
        this->persistentStorageDelegate = &sKvsPersistenStorageDelegate;

        // Group Data provider injection
        sGroupDataProvider.SetStorageDelegate(&sKvsPersistenStorageDelegate);
        ReturnErrorOnFailure(sGroupDataProvider.Init());
        this->groupDataProvider = &sGroupDataProvider;

#if CHIP_CONFIG_ENABLE_SESSION_RESUMPTION
        ReturnErrorOnFailure(sSessionResumptionStorage.Init(&sKvsPersistenStorageDelegate));
        this->sessionResumptionStorage = &sSessionResumptionStorage;
#else
        this->sessionResumptionStorage = nullptr;
#endif

        // Inject access control delegate
        this->accessDelegate = Access::Examples::GetAccessControlDelegate();

        // Inject ACL storage. (Don't initialize it.)
        this->aclStorage = &sAclStorage;

        return CHIP_NO_ERROR;
    }
};

/**
 * The `Server` singleton class is an aggregate for all the resources needed to run a
 * Node that is both Commissionable and mainly used as an end-node with server clusters.
 * In other words, it aggregates the state needed for the type of Node used for most
 * products that are not mainly controller/administrator role.
 *
 * This sington class expects `ServerInitParams` initialization parameters but does not
 * own the resources injected from `ServerInitParams`. Any object pointers/references
 * passed in ServerInitParams must be pre-initialized externally, and shutdown/finalized
 * after `Server::Shutdown()` is called.
 *
 * TODO: Separate lifecycle ownership for some more capabilities that should not belong to
 *       common logic, such as `DispatchShutDownAndStopEventLoop`.
 *
 * TODO: Replace all uses of GetInstance() to "reach in" to this state from all cluster
 *       server common logic that deal with global node state with either a common NodeState
 *       compatible with OperationalDeviceProxy/DeviceProxy, or with injection at common
 *       SDK logic init.
 */
class Server
{
public:
    CHIP_ERROR Init(const ServerInitParams & initParams);

#if CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY_CLIENT
    CHIP_ERROR SendUserDirectedCommissioningRequest(chip::Transport::PeerAddress commissioner);
#endif // CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY_CLIENT

    /**
     * @brief Call this function to rejoin existing groups found in the GroupDataProvider
     */
    void RejoinExistingMulticastGroups();

    FabricTable & GetFabricTable() { return mFabrics; }

    CASESessionManager * GetCASESessionManager() { return &mCASESessionManager; }

    Messaging::ExchangeManager & GetExchangeManager() { return mExchangeMgr; }

    SessionManager & GetSecureSessionManager() { return mSessions; }

    SessionResumptionStorage * GetSessionResumptionStorage() { return mSessionResumptionStorage; }

    TransportMgrBase & GetTransportManager() { return mTransports; }

    Credentials::GroupDataProvider * GetGroupDataProvider() { return mGroupsProvider; }

#if CONFIG_NETWORK_LAYER_BLE
    Ble::BleLayer * GetBleLayerObject() { return mBleLayer; }
#endif

    CommissioningWindowManager & GetCommissioningWindowManager() { return mCommissioningWindowManager; }

    PersistentStorageDelegate & GetPersistentStorage() { return *mDeviceStorage; }

    TestEventTriggerDelegate * GetTestEventTriggerDelegate() { return mTestEventTriggerDelegate; }

    /**
     * This function send the ShutDown event before stopping
     * the event loop.
     */
    void DispatchShutDownAndStopEventLoop();

    void Shutdown();

    void ScheduleFactoryReset();

    static Server & GetInstance() { return sServer; }

private:
    Server() = default;

    static Server sServer;

    class GroupDataProviderListener final : public Credentials::GroupDataProvider::GroupListener
    {
    public:
        GroupDataProviderListener() {}

        CHIP_ERROR Init(ServerTransportMgr * transports)
        {
            VerifyOrReturnError(transports != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

            mTransports = transports;
            return CHIP_NO_ERROR;
        };

        void OnGroupAdded(chip::FabricIndex fabric_index, const Credentials::GroupDataProvider::GroupInfo & new_group) override
        {
            if (mTransports->MulticastGroupJoinLeave(Transport::PeerAddress::Multicast(fabric_index, new_group.group_id), true) !=
                CHIP_NO_ERROR)
            {
                ChipLogError(AppServer, "Unable to listen to group");
            }
        };

        void OnGroupRemoved(chip::FabricIndex fabric_index, const Credentials::GroupDataProvider::GroupInfo & old_group) override
        {
            mTransports->MulticastGroupJoinLeave(Transport::PeerAddress::Multicast(fabric_index, old_group.group_id), false);
        };

    private:
        ServerTransportMgr * mTransports;
    };

    class ServerFabricDelegate final : public chip::FabricTable::Delegate
    {
    public:
        ServerFabricDelegate() {}

        CHIP_ERROR Init(Server * server)
        {
            VerifyOrReturnError(server != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

            mServer = server;
            return CHIP_NO_ERROR;
        };

        void OnFabricDeletedFromStorage(FabricTable & fabricTable, FabricIndex fabricIndex) override
        {
            (void) fabricTable;
            auto & sessionManager = mServer->GetSecureSessionManager();
            sessionManager.FabricRemoved(fabricIndex);

            // Remove all CASE session resumption state
            auto * sessionResumptionStorage = mServer->GetSessionResumptionStorage();
            if (sessionResumptionStorage != nullptr)
            {
                CHIP_ERROR err = sessionResumptionStorage->DeleteAll(fabricIndex);
                if (err != CHIP_NO_ERROR)
                {
                    ChipLogError(AppServer,
                                 "Warning, failed to delete session resumption state for fabric index 0x%x: %" CHIP_ERROR_FORMAT,
                                 static_cast<unsigned>(fabricIndex), err.Format());
                }
            }

            Credentials::GroupDataProvider * groupDataProvider = mServer->GetGroupDataProvider();
            if (groupDataProvider != nullptr)
            {
                CHIP_ERROR err = groupDataProvider->RemoveFabric(fabricIndex);
                if (err != CHIP_NO_ERROR)
                {
                    ChipLogError(AppServer,
                                 "Warning, failed to delete GroupDataProvider state for fabric index 0x%x: %" CHIP_ERROR_FORMAT,
                                 static_cast<unsigned>(fabricIndex), err.Format());
                }
            }
        };

        void OnFabricRetrievedFromStorage(FabricTable & fabricTable, FabricIndex fabricIndex) override
        {
            (void) fabricTable;
            (void) fabricIndex;
        }

        void OnFabricPersistedToStorage(FabricTable & fabricTable, FabricIndex fabricIndex) override
        {
            (void) fabricTable;
            (void) fabricIndex;
        }

    private:
        Server * mServer = nullptr;
    };

#if CONFIG_NETWORK_LAYER_BLE
    Ble::BleLayer * mBleLayer = nullptr;
#endif

    ServerTransportMgr mTransports;
    SessionManager mSessions;
    CASEServer mCASEServer;

    CASESessionManager mCASESessionManager;
    CASEClientPool<CHIP_CONFIG_DEVICE_MAX_ACTIVE_CASE_CLIENTS> mCASEClientPool;
    OperationalDeviceProxyPool<CHIP_CONFIG_DEVICE_MAX_ACTIVE_DEVICES> mDevicePool;

    Messaging::ExchangeManager mExchangeMgr;
    FabricTable mFabrics;
    secure_channel::MessageCounterManager mMessageCounterManager;
#if CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY_CLIENT
    chip::Protocols::UserDirectedCommissioning::UserDirectedCommissioningClient gUDCClient;
#endif // CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY_CLIENT
    CommissioningWindowManager mCommissioningWindowManager;

    PersistentStorageDelegate * mDeviceStorage;
    SessionResumptionStorage * mSessionResumptionStorage;
    Credentials::GroupDataProvider * mGroupsProvider;
    app::DefaultAttributePersistenceProvider mAttributePersister;
    GroupDataProviderListener mListener;
    ServerFabricDelegate mFabricDelegate;

    Access::AccessControl mAccessControl;
    app::AclStorage * mAclStorage;

    TestEventTriggerDelegate * mTestEventTriggerDelegate;

    uint16_t mOperationalServicePort;
    uint16_t mUserDirectedCommissioningPort;
    Inet::InterfaceId mInterfaceId;
};

} // namespace chip
