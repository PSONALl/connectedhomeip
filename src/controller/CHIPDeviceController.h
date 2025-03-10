/*
 *
 *    Copyright (c) 2020-2022 Project CHIP Authors
 *    Copyright (c) 2013-2017 Nest Labs, Inc.
 *    All rights reserved.
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
 *      Declaration of CHIP Device Controller, a common class
 *      that implements connecting and messaging and will later
 *      be expanded to support discovery, pairing and
 *      provisioning of CHIP  devices.
 *
 */

#pragma once

#include <app/CASEClientPool.h>
#include <app/CASESessionManager.h>
#include <app/ClusterStateCache.h>
#include <app/OperationalDeviceProxy.h>
#include <app/OperationalDeviceProxyPool.h>
#include <controller/AbstractDnssdDiscoveryController.h>
#include <controller/AutoCommissioner.h>
#include <controller/CHIPCluster.h>
#include <controller/CHIPDeviceControllerSystemState.h>
#include <controller/CommissioneeDeviceProxy.h>
#include <controller/CommissioningDelegate.h>
#include <controller/DevicePairingDelegate.h>
#include <controller/OperationalCredentialsDelegate.h>
#include <controller/SetUpCodePairer.h>
#include <credentials/FabricTable.h>
#include <credentials/attestation_verifier/DeviceAttestationDelegate.h>
#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>
#include <lib/core/CHIPConfig.h>
#include <lib/core/CHIPCore.h>
#include <lib/core/CHIPPersistentStorageDelegate.h>
#include <lib/core/CHIPTLV.h>
#include <lib/support/DLLUtil.h>
#include <lib/support/Pool.h>
#include <lib/support/SafeInt.h>
#include <lib/support/SerializableIntegerSet.h>
#include <lib/support/Span.h>
#include <lib/support/ThreadOperationalDataset.h>
#include <messaging/ExchangeMgr.h>
#include <protocols/secure_channel/MessageCounterManager.h>
#include <protocols/secure_channel/RendezvousParameters.h>
#include <protocols/user_directed_commissioning/UserDirectedCommissioning.h>
#include <transport/SessionManager.h>
#include <transport/TransportMgr.h>
#include <transport/raw/UDP.h>

#include <controller/CHIPDeviceControllerSystemState.h>

#if CONFIG_DEVICE_LAYER
#include <platform/CHIPDeviceLayer.h>
#endif

#if CONFIG_NETWORK_LAYER_BLE
#include <ble/BleLayer.h>
#endif
#include <controller/DeviceDiscoveryDelegate.h>

namespace chip {

namespace Controller {

using namespace chip::Protocols::UserDirectedCommissioning;

constexpr uint16_t kNumMaxActiveDevices = CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES;

// Raw functions for cluster callbacks
void OnBasicFailure(void * context, CHIP_ERROR err);

struct ControllerInitParams
{
    DeviceControllerSystemState * systemState                       = nullptr;
    DeviceDiscoveryDelegate * deviceDiscoveryDelegate               = nullptr;
    OperationalCredentialsDelegate * operationalCredentialsDelegate = nullptr;

    /* The following keypair must correspond to the public key used for generating
       controllerNOC. It's used by controller to establish CASE sessions with devices */
    Crypto::P256Keypair * operationalKeypair = nullptr;

    /**
     * Controls whether or not the operationalKeypair should be owned by the caller.
     * By default, this is false, but if the keypair cannot be serialized, then
     * setting this to true will allow the caller to manage this keypair's lifecycle.
     */
    bool hasExternallyOwnedOperationalKeypair = false;

    /* The following certificates must be in x509 DER format */
    ByteSpan controllerNOC;
    ByteSpan controllerICAC;
    ByteSpan controllerRCAC;

    //
    // Controls enabling server cluster interactions on a controller. This in turn
    // causes the following to get enabled:
    //
    //  - Advertisement of active controller operational identities.
    //
    bool enableServerInteractions = false;

    chip::VendorId controllerVendorId;
};

struct CommissionerInitParams : public ControllerInitParams
{
    DevicePairingDelegate * pairingDelegate     = nullptr;
    CommissioningDelegate * defaultCommissioner = nullptr;
    // Device attestation verifier instance for the commissioning.
    // If null, the globally set attestation verifier (e.g. from GetDeviceAttestationVerifier()
    // singleton) will be used.
    Credentials::DeviceAttestationVerifier * deviceAttestationVerifier = nullptr;
};

/**
 * @brief
 *   Controller applications can use this class to communicate with already paired CHIP devices. The
 *   application is required to provide access to the persistent storage, where the paired device information
 *   is stored. This object of this class can be initialized with the data from the storage (List of devices,
 *   and device pairing information for individual devices). Alternatively, this class can retrieve the
 *   relevant information when the application tries to communicate with the device
 */
class DLL_EXPORT DeviceController : public AbstractDnssdDiscoveryController
{
public:
    DeviceController();
    ~DeviceController() override {}

    CHIP_ERROR Init(ControllerInitParams params);

    /**
     * @brief
     *  Tears down the entirety of the stack, including destructing key objects in the system.
     *  This expects to be called with external thread synchronization, and will not internally
     *  grab the CHIP stack lock.
     *
     *  This will also not stop the CHIP event queue / thread (if one exists).  Consumers are expected to
     *  ensure this happened before calling this method.
     */
    virtual CHIP_ERROR Shutdown();

    SessionManager * SessionMgr()
    {
        if (mSystemState)
        {
            return mSystemState->SessionMgr();
        }

        return nullptr;
    }

    CHIP_ERROR GetPeerAddressAndPort(PeerId peerId, Inet::IPAddress & addr, uint16_t & port);

    /**
     * This function finds the device corresponding to deviceId, and establishes
     * a CASE session with it.
     *
     * Once the CASE session is successfully established the `onConnectedDevice`
     * callback is called. This can happen before GetConnectedDevice returns if
     * there is an existing CASE session.
     *
     * If a CASE sessions fails to be established, the `onError` callback will
     * be called.  This can also happen before GetConnectedDevice returns.
     *
     * An error return from this function means that neither callback has been
     * called yet, and neither callback will be called in the future.
     */
    CHIP_ERROR GetConnectedDevice(NodeId deviceId, Callback::Callback<OnDeviceConnected> * onConnection,
                                  chip::Callback::Callback<OnDeviceConnectionFailure> * onFailure)
    {
        VerifyOrReturnError(mState == State::Initialized && mFabricInfo != nullptr, CHIP_ERROR_INCORRECT_STATE);
        mSystemState->CASESessionMgr()->FindOrEstablishSession(mFabricInfo->GetPeerIdForNode(deviceId), onConnection, onFailure);
        return CHIP_NO_ERROR;
    }

    /**
     * DEPRECATED - to be removed
     *
     * Forces a DNSSD lookup for the specified device. It finds the corresponding session
     * for the given nodeID and initiates a DNSSD lookup to find/update the node address
     */
    CHIP_ERROR UpdateDevice(NodeId deviceId);

    /**
     * @brief
     *   Compute a PASE verifier and passcode ID for the desired setup pincode.
     *
     *   This can be used to open a commissioning window on the device for
     *   additional administrator commissioning.
     *
     * @param[in] iterations      The number of iterations to use when generating the verifier
     * @param[in] setupPincode    The desired PIN code to use
     * @param[in] salt            The 16-byte salt for verifier computation
     * @param[out] outVerifier    The Spake2pVerifier to be populated on success
     *
     * @return CHIP_ERROR         CHIP_NO_ERROR on success, or corresponding error
     */
    CHIP_ERROR ComputePASEVerifier(uint32_t iterations, uint32_t setupPincode, const ByteSpan & salt,
                                   Spake2pVerifier & outVerifier);

    void RegisterDeviceDiscoveryDelegate(DeviceDiscoveryDelegate * delegate) { mDeviceDiscoveryDelegate = delegate; }

    /**
     * @brief Get the Compressed Fabric ID assigned to the device.
     */
    uint64_t GetCompressedFabricId() const { return mLocalId.GetCompressedFabricId(); }

    /**
     * @brief Get the raw Fabric ID assigned to the device.
     */
    uint64_t GetFabricId() const { return mFabricId; }

    /**
     * @brief Get the Node ID of this instance.
     */
    NodeId GetNodeId() const { return mLocalId.GetNodeId(); }

    CHIP_ERROR GetFabricIndex(FabricIndex * value)
    {
        VerifyOrReturnError(mState == State::Initialized && mFabricInfo != nullptr && value != nullptr, CHIP_ERROR_INCORRECT_STATE);
        *value = mFabricInfo->GetFabricIndex();
        return CHIP_NO_ERROR;
    }

    FabricInfo * GetFabricInfo() { return mFabricInfo; }

    void ReleaseOperationalDevice(NodeId remoteDeviceId);

    OperationalCredentialsDelegate * GetOperationalCredentialsDelegate() { return mOperationalCredentialsDelegate; }

    /**
     * TEMPORARY - DO NOT USE or if you use please request review on why/how to
     * officially support such an API.
     *
     * This was added to support the 'reuse session' logic in cirque integration
     * tests however since that is the only client, the correct update is to
     * use 'ConnectDevice' and wait for connect success/failure inside the CI
     * logic. The current code does not do that because python was not set up
     * to wait for timeouts on success/fail, hence this temporary method.
     *
     * TODO(andy31415): update cirque test and remove this method.
     *
     * Returns success if a session with the given peer does not exist yet.
     */
    CHIP_ERROR DisconnectDevice(NodeId nodeId);

    /**
     * @brief
     *   Reconfigures a new set of operational credentials to be used with this
     *   controller given ControllerInitParams state.
     *
     * WARNING: This is a low-level method that should only be called directly
     *          if you know exactly how this will interact with controller state,
     *          since there are several integrations that do this call for you.
     *          It can be used for fine-grained dependency injection of a controller's
     *          NOC and operational keypair.
     */
    CHIP_ERROR InitControllerNOCChain(const ControllerInitParams & params);

protected:
    enum class State
    {
        NotInitialized,
        Initialized
    };

    State mState;

    PeerId mLocalId          = PeerId();
    FabricId mFabricId       = kUndefinedFabricId;
    FabricInfo * mFabricInfo = nullptr;

    // TODO(cecille): Make this configuarable.
    static constexpr int kMaxCommissionableNodes = 10;
    Dnssd::DiscoveredNodeData mCommissionableNodes[kMaxCommissionableNodes];
    DeviceControllerSystemState * mSystemState = nullptr;

    ControllerDeviceInitParams GetControllerDeviceInitParams();

    OperationalCredentialsDelegate * mOperationalCredentialsDelegate;

    chip::VendorId mVendorId;

    /// Fetches the session to use for the current device. Allows overriding
    /// in case subclasses want to create the session if it does not yet exist
    virtual OperationalDeviceProxy * GetDeviceSession(const PeerId & peerId);

    DiscoveredNodeList GetDiscoveredNodes() override { return DiscoveredNodeList(mCommissionableNodes); }

private:
    void ReleaseOperationalDevice(OperationalDeviceProxy * device);
};

#if CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY
using UdcTransportMgr = TransportMgr<Transport::UDP /* IPv6 */
#if INET_CONFIG_ENABLE_IPV4
                                     ,
                                     Transport::UDP /* IPv4 */
#endif
                                     >;
#endif

/**
 * @brief
 *   The commissioner applications can use this class to pair new/unpaired CHIP devices. The application is
 *   required to provide write access to the persistent storage, where the paired device information
 *   will be stored.
 */
class DLL_EXPORT DeviceCommissioner : public DeviceController,
#if CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY // make this commissioner discoverable
                                      public Protocols::UserDirectedCommissioning::InstanceNameResolver,
#endif
                                      public SessionEstablishmentDelegate,
                                      public app::ClusterStateCache::Callback
{
public:
    DeviceCommissioner();
    ~DeviceCommissioner() override {}

#if CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY // make this commissioner discoverable
    /**
     * Set port for User Directed Commissioning
     */
    CHIP_ERROR SetUdcListenPort(uint16_t listenPort);
#endif // CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY

    /**
     * Commissioner-specific initialization, includes parameters such as the pairing delegate.
     */
    CHIP_ERROR Init(CommissionerInitParams params);

    /**
     * @brief
     *  Tears down the entirety of the stack, including destructing key objects in the system.
     *  This is not a thread-safe API, and should be called with external synchronization.
     *
     *  Please see implementation for more details.
     */
    CHIP_ERROR Shutdown() override;

    // ----- Connection Management -----
    /**
     * @brief
     *   Pair a CHIP device with the provided code. The code can be either a QRCode
     *   or a Manual Setup Code.
     *   Use registered DevicePairingDelegate object to receive notifications on
     *   pairing status updates.
     *
     *   Note: Pairing process requires that the caller has registered PersistentStorageDelegate
     *         in the Init() call.
     *
     * @param[in] remoteDeviceId        The remote device Id.
     * @param[in] setUpCode             The setup code for connecting to the device
     */
    CHIP_ERROR PairDevice(NodeId remoteDeviceId, const char * setUpCode);
    CHIP_ERROR PairDevice(NodeId remoteDeviceId, const char * setUpCode, const CommissioningParameters & CommissioningParameters);

    /**
     * @brief
     *   Pair a CHIP device with the provided Rendezvous connection parameters.
     *   Use registered DevicePairingDelegate object to receive notifications on
     *   pairing status updates.
     *
     *   Note: Pairing process requires that the caller has registered PersistentStorageDelegate
     *         in the Init() call.
     *
     * @param[in] remoteDeviceId        The remote device Id.
     * @param[in] rendezvousParams      The Rendezvous connection parameters
     */
    CHIP_ERROR PairDevice(NodeId remoteDeviceId, RendezvousParameters & rendezvousParams);
    /**
     * @overload
     * @param[in] remoteDeviceId        The remote device Id.
     * @param[in] rendezvousParams      The Rendezvous connection parameters
     * @param[in] commissioningParams    The commissioning parameters (uses default if not supplied)
     */
    CHIP_ERROR PairDevice(NodeId remoteDeviceId, RendezvousParameters & rendezvousParams,
                          CommissioningParameters & commissioningParams);

    /**
     * @brief
     *   Start establishing a PASE connection with a node for the purposes of commissioning.
     *   Commissioners that wish to use the auto-commissioning functions should use the
     *   supplied "PairDevice" functions above to automatically establish a connection then
     *   perform commissioning. This function is intended to be use by commissioners that
     *   are not using the supplied auto-commissioner.
     *
     *   This function is non-blocking. PASE is established once the DevicePairingDelegate
     *   receives the OnPairingComplete call.
     *
     *   PASE connections can only be established with nodes that have their commissioning
     *   window open. The PASE connection will fail if this window is not open and the
     *   OnPairingComplete will be called with an error.
     *
     * @param[in] remoteDeviceId        The remote device Id.
     * @param[in] params                The Rendezvous connection parameters
     */
    CHIP_ERROR EstablishPASEConnection(NodeId remoteDeviceId, RendezvousParameters & params);

    /**
     * @brief
     *   Start establishing a PASE connection with a node for the purposes of commissioning.
     *   Commissioners that wish to use the auto-commissioning functions should use the
     *   supplied "PairDevice" functions above to automatically establish a connection then
     *   perform commissioning. This function is intended to be used by commissioners that
     *   are not using the supplied auto-commissioner.
     *
     *   This function is non-blocking. PASE is established once the DevicePairingDelegate
     *   receives the OnPairingComplete call.
     *
     *   PASE connections can only be established with nodes that have their commissioning
     *   window open. The PASE connection will fail if this window is not open and in that case
     *   OnPairingComplete will be called with an error.
     *
     * @param[in] remoteDeviceId        The remote device Id.
     * @param[in] setUpCode             The setup code for connecting to the device
     */
    CHIP_ERROR EstablishPASEConnection(NodeId remoteDeviceId, const char * setUpCode);

    /**
     * @brief
     *   Start the auto-commissioning process on a node after establishing a PASE connection.
     *   This function is intended to be used in conjunction with the EstablishPASEConnection
     *   function. It can be called either before or after the DevicePairingDelegate receives
     *   the OnPairingComplete call. Commissioners that want to perform simple auto-commissioning
     *   should use the supplied "PairDevice" functions above, which will establish the PASE
     *   connection and commission automatically.
     *
     * @param[in] remoteDeviceId        The remote device Id.
     * @param[in] params                The commissioning parameters
     */
    CHIP_ERROR Commission(NodeId remoteDeviceId, CommissioningParameters & params);
    CHIP_ERROR Commission(NodeId remoteDeviceId);

    /**
     * @brief
     *   This function instructs the commissioner to proceed to the next stage of commissioning after
     *   attestation failure is reported to an installed attestation delegate.
     *
     * @param[in] device                The device being commissioned.
     * @param[in] attestationResult     The attestation result to use instead of whatever the device
     *                                  attestation verifier came up with. May be a success or an error result.
     */
    CHIP_ERROR
    ContinueCommissioningAfterDeviceAttestationFailure(DeviceProxy * device,
                                                       Credentials::AttestationVerificationResult attestationResult);

    CHIP_ERROR GetDeviceBeingCommissioned(NodeId deviceId, CommissioneeDeviceProxy ** device);

    /**
     * @brief
     *   This function stops a pairing process that's in progress. It does not delete the pairing of a previously
     *   paired device.
     *
     * @param[in] remoteDeviceId        The remote device Id.
     *
     * @return CHIP_ERROR               CHIP_NO_ERROR on success, or corresponding error
     */
    CHIP_ERROR StopPairing(NodeId remoteDeviceId);

    /**
     * @brief
     *   Remove pairing for a paired device. If the device is currently being paired, it'll stop the pairing process.
     *
     * @param[in] remoteDeviceId        The remote device Id.
     *
     * @return CHIP_ERROR               CHIP_NO_ERROR on success, or corresponding error
     */
    CHIP_ERROR UnpairDevice(NodeId remoteDeviceId);

    //////////// SessionEstablishmentDelegate Implementation ///////////////
    void OnSessionEstablishmentError(CHIP_ERROR error) override;
    void OnSessionEstablished(const SessionHandle & session) override;

    void RendezvousCleanup(CHIP_ERROR status);

    void PerformCommissioningStep(DeviceProxy * device, CommissioningStage step, CommissioningParameters & params,
                                  CommissioningDelegate * delegate, EndpointId endpoint, Optional<System::Clock::Timeout> timeout);

    /**
     * @brief
     *   This function validates the Attestation Information sent by the device.
     *
     * @param[in] info Structure contatining all the required information for validating the device attestation.
     */
    CHIP_ERROR ValidateAttestationInfo(const Credentials::DeviceAttestationVerifier::AttestationInfo & info);

    /**
     * @brief
     * Sends CommissioningStepComplete report to the commissioning delegate. Function will fill in current step.
     * @params[in] err      error from the current step
     * @params[in] report   report to send. Current step will be filled in automatically
     */
    void
    CommissioningStageComplete(CHIP_ERROR err,
                               CommissioningDelegate::CommissioningReport report = CommissioningDelegate::CommissioningReport());

#if CONFIG_NETWORK_LAYER_BLE
#if CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE
    /**
     * @brief
     *   Prior to commissioning, the Controller should make sure the BleLayer transport
     *   is set to the Commissioner transport and not the Server transport.
     */
    void ConnectBleTransportToSelf();
#endif // CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE

    /**
     * @brief
     *   Once we have finished all commissioning work, the Controller should close the BLE
     *   connection to the device and establish CASE session / another PASE session to the device
     *   if needed.
     * @return CHIP_ERROR   The return status
     */
    CHIP_ERROR CloseBleConnection();
#endif
    /**
     * @brief
     *   Discover all devices advertising as commissionable.
     *   Should be called on main loop thread.
     * * @param[in] filter  Browse filter - controller will look for only the specified subtype.
     * @return CHIP_ERROR   The return status
     */
    CHIP_ERROR DiscoverCommissionableNodes(Dnssd::DiscoveryFilter filter);

    /**
     * @brief
     *   Returns information about discovered devices.
     *   Should be called on main loop thread.
     * @return const DiscoveredNodeData* info about the selected device. May be nullptr if no information has been returned yet.
     */
    const Dnssd::DiscoveredNodeData * GetDiscoveredDevice(int idx);

    /**
     * @brief
     *   Returns the max number of commissionable nodes this commissioner can track mdns information for.
     * @return int  The max number of commissionable nodes supported
     */
    int GetMaxCommissionableNodesSupported() { return kMaxCommissionableNodes; }

#if CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY // make this commissioner discoverable
    /**
     * @brief
     *   Called when a UDC message is received specifying the given instanceName
     * This method indicates that UDC Server needs the Commissionable Node corresponding to
     * the given instance name to be found. UDC Server will wait for OnCommissionableNodeFound.
     *
     * @param instanceName DNS-SD instance name for the client requesting commissioning
     *
     */
    void FindCommissionableNode(char * instanceName) override;

    /**
     * @brief
     *   Return the UDC Server instance
     *
     */
    UserDirectedCommissioningServer * GetUserDirectedCommissioningServer() { return mUdcServer; }
#endif // CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY

    /**
     * @brief
     *   Overrides method from AbstractDnssdDiscoveryController
     *
     * @param nodeData DNS-SD node information
     *
     */
    void OnNodeDiscovered(const chip::Dnssd::DiscoveredNodeData & nodeData) override;

    void RegisterPairingDelegate(DevicePairingDelegate * pairingDelegate) { mPairingDelegate = pairingDelegate; }
    DevicePairingDelegate * GetPairingDelegate() const { return mPairingDelegate; }

    // ClusterStateCache::Callback impl
    void OnDone(app::ReadClient *) override;

    // Commissioner will establish new device connections after PASE.
    OperationalDeviceProxy * GetDeviceSession(const PeerId & peerId) override;

private:
    DevicePairingDelegate * mPairingDelegate;

    DeviceProxy * mDeviceBeingCommissioned               = nullptr;
    CommissioneeDeviceProxy * mDeviceInPASEEstablishment = nullptr;

    CommissioningStage mCommissioningStage = CommissioningStage::kSecurePairing;
    bool mRunCommissioningAfterConnection  = false;

    ObjectPool<CommissioneeDeviceProxy, kNumMaxActiveDevices> mCommissioneeDevicePool;

#if CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY // make this commissioner discoverable
    UserDirectedCommissioningServer * mUdcServer = nullptr;
    // mUdcTransportMgr is for insecure communication (ex. user directed commissioning)
    UdcTransportMgr * mUdcTransportMgr = nullptr;
    uint16_t mUdcListenPort            = CHIP_UDC_PORT;
#endif // CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY

    CHIP_ERROR LoadKeyId(PersistentStorageDelegate * delegate, uint16_t & out);

    void OnSessionEstablishmentTimeout();

    static void OnSessionEstablishmentTimeoutCallback(System::Layer * aLayer, void * aAppState);

    /* This function sends a Device Attestation Certificate chain request to the device.
       The function does not hold a reference to the device object.
     */
    CHIP_ERROR SendCertificateChainRequestCommand(DeviceProxy * device, Credentials::CertificateType certificateType);
    /* This function sends an Attestation request to the device.
       The function does not hold a reference to the device object.
     */
    CHIP_ERROR SendAttestationRequestCommand(DeviceProxy * device, const ByteSpan & attestationNonce);
    /* This function sends an CSR request to the device.
       The function does not hold a reference to the device object.
     */
    CHIP_ERROR SendOperationalCertificateSigningRequestCommand(DeviceProxy * device, const ByteSpan & csrNonce);
    /* This function sends the operational credentials to the device.
       The function does not hold a reference to the device object.
     */
    CHIP_ERROR SendOperationalCertificate(DeviceProxy * device, const ByteSpan & nocCertBuf, const Optional<ByteSpan> & icaCertBuf,
                                          AesCcm128KeySpan ipk, NodeId adminSubject);
    /* This function sends the trusted root certificate to the device.
       The function does not hold a reference to the device object.
     */
    CHIP_ERROR SendTrustedRootCertificate(DeviceProxy * device, const ByteSpan & rcac);

    /* This function is called by the commissioner code when the device completes
       the operational credential provisioning process.
       The function does not hold a reference to the device object.
       */
    CHIP_ERROR OnOperationalCredentialsProvisioningCompletion(DeviceProxy * device);

    /* Callback when the previously sent CSR request results in failure */
    static void OnCSRFailureResponse(void * context, CHIP_ERROR error);

    void ExtendArmFailSafeForFailedDeviceAttestation(Credentials::AttestationVerificationResult result);
    static void OnCertificateChainFailureResponse(void * context, CHIP_ERROR error);
    static void OnCertificateChainResponse(
        void * context, const app::Clusters::OperationalCredentials::Commands::CertificateChainResponse::DecodableType & response);

    static void OnAttestationFailureResponse(void * context, CHIP_ERROR error);
    static void
    OnAttestationResponse(void * context,
                          const app::Clusters::OperationalCredentials::Commands::AttestationResponse::DecodableType & data);

    /**
     * @brief
     *   This function is called by the IM layer when the commissioner receives the CSR from the device.
     *   (Reference: Specifications section 11.18.5.6. NOCSR Elements)
     *
     * @param[in] context               The context provided while registering the callback.
     * @param[in] data                  The response struct containing the following fields:
     *                                    NOCSRElements: CSR elements as per specifications section 11.22.5.6. NOCSR Elements.
     *                                    AttestationSignature: Cryptographic signature generated for the fields in the response
     * message.
     */
    static void OnOperationalCertificateSigningRequest(
        void * context, const app::Clusters::OperationalCredentials::Commands::CSRResponse::DecodableType & data);

    /* Callback when adding operational certs to device results in failure */
    static void OnAddNOCFailureResponse(void * context, CHIP_ERROR errro);
    /* Callback when the device confirms that it has added the operational certificates */
    static void
    OnOperationalCertificateAddResponse(void * context,
                                        const app::Clusters::OperationalCredentials::Commands::NOCResponse::DecodableType & data);

    /* Callback when the device confirms that it has added the root certificate */
    static void OnRootCertSuccessResponse(void * context, const chip::app::DataModel::NullObjectType &);
    /* Callback called when adding root cert to device results in failure */
    static void OnRootCertFailureResponse(void * context, CHIP_ERROR error);

    static void OnDeviceConnectedFn(void * context, OperationalDeviceProxy * device);
    static void OnDeviceConnectionFailureFn(void * context, PeerId peerId, CHIP_ERROR error);

    static void OnDeviceAttestationInformationVerification(void * context, Credentials::AttestationVerificationResult result);

    static void OnDeviceNOCChainGeneration(void * context, CHIP_ERROR status, const ByteSpan & noc, const ByteSpan & icac,
                                           const ByteSpan & rcac, Optional<AesCcm128KeySpan> ipk, Optional<NodeId> adminSubject);
    static void OnArmFailSafe(void * context,
                              const chip::app::Clusters::GeneralCommissioning::Commands::ArmFailSafeResponse::DecodableType & data);
    static void OnSetRegulatoryConfigResponse(
        void * context,
        const chip::app::Clusters::GeneralCommissioning::Commands::SetRegulatoryConfigResponse::DecodableType & data);
    static void
    OnNetworkConfigResponse(void * context,
                            const chip::app::Clusters::NetworkCommissioning::Commands::NetworkConfigResponse::DecodableType & data);
    static void OnConnectNetworkResponse(
        void * context, const chip::app::Clusters::NetworkCommissioning::Commands::ConnectNetworkResponse::DecodableType & data);
    static void OnCommissioningCompleteResponse(
        void * context,
        const chip::app::Clusters::GeneralCommissioning::Commands::CommissioningCompleteResponse::DecodableType & data);
    static void OnDisarmFailsafe(void * context,
                                 const app::Clusters::GeneralCommissioning::Commands::ArmFailSafeResponse::DecodableType & data);
    static void OnDisarmFailsafeFailure(void * context, CHIP_ERROR error);
    void DisarmDone();
    static void OnArmFailSafeExtendedForFailedDeviceAttestation(
        void * context, const chip::app::Clusters::GeneralCommissioning::Commands::ArmFailSafeResponse::DecodableType & data);
    static void OnFailedToExtendedArmFailSafeFailedDeviceAttestation(void * context, CHIP_ERROR error);

    /**
     * @brief
     *   This function processes the CSR sent by the device.
     *   (Reference: Specifications section 11.18.5.6. NOCSR Elements)
     *
     * @param[in] proxy           device proxy
     * @param[in] NOCSRElements   CSR elements as per specifications section 11.22.5.6. NOCSR Elements.
     * @param[in] AttestationSignature       Cryptographic signature generated for all the above fields.
     * @param[in] dac               device attestation certificate
     * @param[in] pai               Product Attestation Intermediate certificate
     * @param[in] csrNonce          certificate signing request nonce
     */
    CHIP_ERROR ProcessCSR(DeviceProxy * proxy, const ByteSpan & NOCSRElements, const ByteSpan & AttestationSignature,
                          const ByteSpan & dac, const ByteSpan & pai, const ByteSpan & csrNonce);

    /**
     * @brief
     *   This function validates the CSR information from the device.
     *   (Reference: Specifications section 11.18.5.6. NOCSR Elements)
     *
     * @param[in] proxy           device proxy
     * @param[in] NOCSRElements   CSR elements as per specifications section 11.22.5.6. NOCSR Elements.
     * @param[in] AttestationSignature       Cryptographic signature generated for all the above fields.
     * @param[in] dac               device attestation certificate
     * @param[in] csrNonce          certificate signing request nonce
     */
    CHIP_ERROR ValidateCSR(DeviceProxy * proxy, const ByteSpan & NOCSRElements, const ByteSpan & AttestationSignature,
                           const ByteSpan & dac, const ByteSpan & csrNonce);

    /**
     * @brief
     *   This function processes the DAC or PAI certificate sent by the device.
     */
    CHIP_ERROR ProcessCertificateChain(const ByteSpan & certificate);

    void HandleAttestationResult(CHIP_ERROR err);

    CommissioneeDeviceProxy * FindCommissioneeDevice(NodeId id);
    CommissioneeDeviceProxy * FindCommissioneeDevice(const Transport::PeerAddress & peerAddress);
    void ReleaseCommissioneeDevice(CommissioneeDeviceProxy * device);

    template <typename ClusterObjectT, typename RequestObjectT>
    CHIP_ERROR SendCommand(DeviceProxy * device, const RequestObjectT & request,
                           CommandResponseSuccessCallback<typename RequestObjectT::ResponseType> successCb,
                           CommandResponseFailureCallback failureCb)
    {
        return SendCommand<ClusterObjectT>(device, request, successCb, failureCb, 0, NullOptional);
    }

    template <typename ClusterObjectT, typename RequestObjectT>
    CHIP_ERROR SendCommand(DeviceProxy * device, const RequestObjectT & request,
                           CommandResponseSuccessCallback<typename RequestObjectT::ResponseType> successCb,
                           CommandResponseFailureCallback failureCb, EndpointId endpoint, Optional<System::Clock::Timeout> timeout)
    {
        ClusterObjectT cluster;
        cluster.Associate(device, endpoint);
        cluster.SetCommandTimeout(timeout);

        return cluster.InvokeCommand(request, this, successCb, failureCb);
    }

    static CHIP_ERROR ConvertFromOperationalCertStatus(chip::app::Clusters::OperationalCredentials::OperationalCertStatus err);

    // Sends commissioning complete callbacks to the delegate depending on the status. Sends
    // OnCommissioningComplete and either OnCommissioningSuccess or OnCommissioningFailure depending on the given completion status.
    void SendCommissioningCompleteCallbacks(NodeId nodeId, const CompletionStatus & completionStatus);

    // Cleans up and resets failsafe as appropriate depending on the error and the failed stage.
    // For success, sends completion report with the CommissioningDelegate and sends callbacks to the PairingDelegate
    // For failures after AddNOC succeeds, sends completion report with the CommissioningDelegate and sends callbacks to the
    // PairingDelegate. In this case, it does not disarm the failsafe or close the pase connection. For failures up through AddNOC,
    // sends a command to immediately expire the failsafe, then sends completion report with the CommissioningDelegate and callbacks
    // to the PairingDelegate upon arm failsafe command completion.
    void CleanupCommissioning(DeviceProxy * proxy, NodeId nodeId, const CompletionStatus & completionStatus);

    chip::Callback::Callback<OnDeviceConnected> mOnDeviceConnectedCallback;
    chip::Callback::Callback<OnDeviceConnectionFailure> mOnDeviceConnectionFailureCallback;

    chip::Callback::Callback<Credentials::OnAttestationInformationVerification> mDeviceAttestationInformationVerificationCallback;

    chip::Callback::Callback<OnNOCChainGeneration> mDeviceNOCChainCallback;
    SetUpCodePairer mSetUpCodePairer;
    AutoCommissioner mAutoCommissioner;
    CommissioningDelegate * mDefaultCommissioner =
        nullptr; // Commissioning delegate to call when PairDevice / Commission functions are used
    CommissioningDelegate * mCommissioningDelegate =
        nullptr; // Commissioning delegate that issued the PerformCommissioningStep command
    CompletionStatus commissioningCompletionStatus;

    Platform::UniquePtr<app::ClusterStateCache> mAttributeCache;
    Platform::UniquePtr<app::ReadClient> mReadClient;
    Credentials::AttestationVerificationResult mAttestationResult;
    Credentials::DeviceAttestationVerifier * mDeviceAttestationVerifier = nullptr;
};

} // namespace Controller
} // namespace chip
