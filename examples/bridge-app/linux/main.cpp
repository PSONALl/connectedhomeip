/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
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

#include <AppMain.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>

#include <app-common/zap-generated/af-structs.h>

#include <app-common/zap-generated/attribute-id.h>
#include <app-common/zap-generated/cluster-id.h>
#include <app/chip-zcl-zpro-codec.h>
#include <app/reporting/reporting.h>
#include <app/util/af-types.h>
#include <app/util/af.h>
#include <app/util/attribute-storage.h>
#include <app/util/util.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/core/CHIPError.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/ZclString.h>
#include <platform/CommissionableDataProvider.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>

#include "CommissionableInit.h"
#include "Device.h"
#include <app/server/Server.h>

#include <cassert>
#include <iostream>

using namespace chip;
using namespace chip::Credentials;
using namespace chip::Inet;
using namespace chip::Transport;
using namespace chip::DeviceLayer;

namespace {

const int kNodeLabelSize = 32;
// Current ZCL implementation of Struct uses a max-size array of 254 bytes
const int kDescriptorAttributeArraySize = 254;
const int kFixedLabelAttributeArraySize = 254;
// Four attributes in descriptor cluster: DeviceTypeList, ServerList, ClientList, PartsList
const int kFixedLabelElementsOctetStringSize = 16;

EndpointId gCurrentEndpointId;
EndpointId gFirstDynamicEndpointId;
Device * gDevices[CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT];

// ENDPOINT DEFINITIONS:
// =================================================================================
//
// Endpoint definitions will be reused across multiple endpoints for every instance of the
// endpoint type.
// There will be no intrinsic storage for the endpoint attributes declared here.
// Instead, all attributes will be treated as EXTERNAL, and therefore all reads
// or writes to the attributes must be handled within the emberAfExternalAttributeWriteCallback
// and emberAfExternalAttributeReadCallback functions declared herein. This fits
// the typical model of a bridge, since a bridge typically maintains its own
// state database representing the devices connected to it.

// Device types for dynamic endpoints: TODO Need a generated file from ZAP to define these!
// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGED_NODE 0x0013
// (taken from lo-devices.xml)
#define DEVICE_TYPE_LO_ON_OFF_LIGHT 0x0100
// (taken from lo-devices.xml)
#define DEVICE_TYPE_LO_ON_OFF_LIGHT_SWITCH 0x0103
// (taken from chip-devices.xml)
#define DEVICE_TYPE_ROOT_NODE 0x0016
// (taken from chip-devices.xml)
#define DEVICE_TYPE_BRIDGE 0x000e
// (taken from chip-devices.xml)
#define DEVICE_TYPE_POWER_SOURCE 0x0011

// Device Version for dynamic endpoints:
#define DEVICE_VERSION_DEFAULT 1

// ---------------------------------------------------------------------------
//
// LIGHT ENDPOINT: contains the following clusters:
//   - On/Off
//   - Descriptor
//   - Bridged Device Basic
//   - Fixed Label

// Declare On/Off cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(onOffAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_ON_OFF_ATTRIBUTE_ID, BOOLEAN, 1, 0), /* on/off */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Descriptor cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(descriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_DEVICE_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0),     /* device list */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_SERVER_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0), /* server list */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_CLIENT_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0), /* client list */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_PARTS_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0),  /* parts list */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Bridged Device Basic information cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(bridgedDeviceBasicAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_NODE_LABEL_ATTRIBUTE_ID, CHAR_STRING, kNodeLabelSize, 0), /* NodeLabel */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_REACHABLE_ATTRIBUTE_ID, BOOLEAN, 1, 0),               /* Reachable */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Fixed Label cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(fixedLabelAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_LABEL_LIST_ATTRIBUTE_ID, ARRAY, kFixedLabelAttributeArraySize, 0), /* label list */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Cluster List for Bridged Light endpoint
// TODO: It's not clear whether it would be better to get the command lists from
// the ZAP config on our last fixed endpoint instead.
constexpr CommandId onOffIncomingCommands[] = {
    app::Clusters::OnOff::Commands::Off::Id,
    app::Clusters::OnOff::Commands::On::Id,
    app::Clusters::OnOff::Commands::Toggle::Id,
    app::Clusters::OnOff::Commands::OffWithEffect::Id,
    app::Clusters::OnOff::Commands::OnWithRecallGlobalScene::Id,
    app::Clusters::OnOff::Commands::OnWithTimedOff::Id,
    kInvalidCommandId,
};

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedLightClusters)
DECLARE_DYNAMIC_CLUSTER(ZCL_ON_OFF_CLUSTER_ID, onOffAttrs, onOffIncomingCommands, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ZCL_DESCRIPTOR_CLUSTER_ID, descriptorAttrs, nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID, bridgedDeviceBasicAttrs, nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ZCL_FIXED_LABEL_CLUSTER_ID, fixedLabelAttrs, nullptr, nullptr), DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Light endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedLightEndpoint, bridgedLightClusters);
DataVersion gLight1DataVersions[ArraySize(bridgedLightClusters)];
DataVersion gLight2DataVersions[ArraySize(bridgedLightClusters)];
DataVersion gLight3DataVersions[ArraySize(bridgedLightClusters)];
DataVersion gLight4DataVersions[ArraySize(bridgedLightClusters)];

// ---------------------------------------------------------------------------
//
// SWITCH ENDPOINT: contains the following clusters:
//   - Switch
//   - Descriptor
//   - Bridged Device Basic
//   - Fixed Label

// Declare Switch cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(switchAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_NUMBER_OF_POSITIONS_ATTRIBUTE_ID, INT8U, 1, 0),       /* NumberOfPositions */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_CURRENT_POSITION_ATTRIBUTE_ID, INT8U, 1, 0),      /* CurrentPosition */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_MULTI_PRESS_MAX_ATTRIBUTE_ID, INT8U, 1, 0),       /* MultiPressMax */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_FEATURE_MAP_SERVER_ATTRIBUTE_ID, BITMAP32, 4, 0), /* FeatureMap */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Descriptor cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(switchDescriptorAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_DEVICE_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0),     /* device list */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_SERVER_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0), /* server list */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_CLIENT_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0), /* client list */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_PARTS_LIST_ATTRIBUTE_ID, ARRAY, kDescriptorAttributeArraySize, 0),  /* parts list */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Bridged Device Basic information cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(switchBridgedDeviceBasicAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_NODE_LABEL_ATTRIBUTE_ID, CHAR_STRING, kNodeLabelSize, 0), /* NodeLabel */
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_REACHABLE_ATTRIBUTE_ID, BOOLEAN, 1, 0),               /* Reachable */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Fixed Label cluster attributes
DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(switchFixedLabelAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_LABEL_LIST_ATTRIBUTE_ID, ARRAY, kFixedLabelAttributeArraySize, 0), /* label list */
    DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

// Declare Cluster List for Bridged Switch endpoint
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedSwitchClusters)
DECLARE_DYNAMIC_CLUSTER(ZCL_SWITCH_CLUSTER_ID, switchAttrs, nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ZCL_DESCRIPTOR_CLUSTER_ID, switchDescriptorAttrs, nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID, switchBridgedDeviceBasicAttrs, nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ZCL_FIXED_LABEL_CLUSTER_ID, switchFixedLabelAttrs, nullptr, nullptr) DECLARE_DYNAMIC_CLUSTER_LIST_END;

// Declare Bridged Switch endpoint
DECLARE_DYNAMIC_ENDPOINT(bridgedSwitchEndpoint, bridgedSwitchClusters);
DataVersion gSwitch1DataVersions[ArraySize(bridgedSwitchClusters)];
DataVersion gSwitch2DataVersions[ArraySize(bridgedSwitchClusters)];

// ---------------------------------------------------------------------------
//
// POWER SOURCE ENDPOINT: contains the following clusters:
//   - Power Source
//   - Descriptor
//   - Bridged Device Basic

DECLARE_DYNAMIC_ATTRIBUTE_LIST_BEGIN(powerSourceAttrs)
DECLARE_DYNAMIC_ATTRIBUTE(ZCL_POWER_SOURCE_BAT_CHARGE_LEVEL_ATTRIBUTE_ID, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_POWER_SOURCE_ORDER_ATTRIBUTE_ID, INT8U, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_POWER_SOURCE_STATUS_ATTRIBUTE_ID, ENUM8, 1, 0),
    DECLARE_DYNAMIC_ATTRIBUTE(ZCL_POWER_SOURCE_DESCRIPTION_ATTRIBUTE_ID, CHAR_STRING, 32, 0), DECLARE_DYNAMIC_ATTRIBUTE_LIST_END();

DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedPowerSourceClusters)
DECLARE_DYNAMIC_CLUSTER(ZCL_DESCRIPTOR_CLUSTER_ID, descriptorAttrs, nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID, bridgedDeviceBasicAttrs, nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ZCL_POWER_SOURCE_CLUSTER_ID, powerSourceAttrs, nullptr, nullptr), DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedPowerSourceEndpoint, bridgedPowerSourceClusters);

// ---------------------------------------------------------------------------
//
// COMPOSED DEVICE ENDPOINT: contains the following clusters:
//   - Descriptor
//   - Bridged Device Basic

// Composed Device Configuration
DECLARE_DYNAMIC_CLUSTER_LIST_BEGIN(bridgedComposedDeviceClusters)
DECLARE_DYNAMIC_CLUSTER(ZCL_DESCRIPTOR_CLUSTER_ID, descriptorAttrs, nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER(ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID, bridgedDeviceBasicAttrs, nullptr, nullptr),
    DECLARE_DYNAMIC_CLUSTER_LIST_END;

DECLARE_DYNAMIC_ENDPOINT(bridgedComposedDeviceEndpoint, bridgedComposedDeviceClusters);
DataVersion gComposedDeviceDataVersions[ArraySize(bridgedComposedDeviceClusters)];
DataVersion gComposedSwitch1DataVersions[ArraySize(bridgedSwitchClusters)];
DataVersion gComposedSwitch2DataVersions[ArraySize(bridgedSwitchClusters)];
DataVersion gComposedPowerSourceDataVersions[ArraySize(bridgedPowerSourceClusters)];

} // namespace

// REVISION DEFINITIONS:
// =================================================================================

#define ZCL_DESCRIPTOR_CLUSTER_REVISION (1u)
#define ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_REVISION (1u)
#define ZCL_FIXED_LABEL_CLUSTER_REVISION (1u)
#define ZCL_ON_OFF_CLUSTER_REVISION (4u)
#define ZCL_SWITCH_CLUSTER_REVISION (1u)
#define ZCL_POWER_SOURCE_CLUSTER_REVISION (1u)

// ---------------------------------------------------------------------------

int AddDeviceEndpoint(Device * dev, EmberAfEndpointType * ep, const Span<const EmberAfDeviceType> & deviceTypeList,
                      const Span<DataVersion> & dataVersionStorage, chip::EndpointId parentEndpointId = chip::kInvalidEndpointId)
{
    uint8_t index = 0;
    while (index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        if (nullptr == gDevices[index])
        {
            gDevices[index] = dev;
            EmberAfStatus ret;
            while (1)
            {
                dev->SetEndpointId(gCurrentEndpointId);
                ret =
                    emberAfSetDynamicEndpoint(index, gCurrentEndpointId, ep, dataVersionStorage, deviceTypeList, parentEndpointId);
                if (ret == EMBER_ZCL_STATUS_SUCCESS)
                {
                    ChipLogProgress(DeviceLayer, "Added device %s to dynamic endpoint %d (index=%d)", dev->GetName(),
                                    gCurrentEndpointId, index);
                    return index;
                }
                if (ret != EMBER_ZCL_STATUS_DUPLICATE_EXISTS)
                {
                    return -1;
                }
                // Handle wrap condition
                if (++gCurrentEndpointId < gFirstDynamicEndpointId)
                {
                    gCurrentEndpointId = gFirstDynamicEndpointId;
                }
            }
        }
        index++;
    }
    ChipLogProgress(DeviceLayer, "Failed to add dynamic endpoint: No endpoints available!");
    return -1;
}

int RemoveDeviceEndpoint(Device * dev)
{
    uint8_t index = 0;
    while (index < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        if (gDevices[index] == dev)
        {
            EndpointId ep   = emberAfClearDynamicEndpoint(index);
            gDevices[index] = nullptr;
            ChipLogProgress(DeviceLayer, "Removed device %s from dynamic endpoint %d (index=%d)", dev->GetName(), ep, index);
            // Silence complaints about unused ep when progress logging
            // disabled.
            UNUSED_VAR(ep);
            return index;
        }
        index++;
    }
    return -1;
}

void EncodeFixedLabel(const char * label, const char * value, uint8_t * buffer, uint16_t length,
                      const EmberAfAttributeMetadata * am)
{
    char zclOctetStrBuf[kFixedLabelElementsOctetStringSize];
    _LabelStruct labelStruct;

    // TODO: This size is obviously wrong.  See
    // https://github.com/project-chip/connectedhomeip/issues/10743
    labelStruct.label = CharSpan(label, kFixedLabelElementsOctetStringSize);

    strncpy(zclOctetStrBuf, value, sizeof(zclOctetStrBuf));
    // TODO: This size is obviously wrong.  See
    // https://github.com/project-chip/connectedhomeip/issues/10743
    labelStruct.value = CharSpan(&zclOctetStrBuf[0], sizeof(zclOctetStrBuf));

    // TODO: Need to set up an AttributeAccessInterface to handle the lists here.
}

void HandleDeviceStatusChanged(Device * dev, Device::Changed_t itemChangedMask)
{
    if (itemChangedMask & Device::kChanged_Reachable)
    {
        uint8_t reachable = dev->IsReachable() ? 1 : 0;
        MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID,
                                               ZCL_REACHABLE_ATTRIBUTE_ID, ZCL_BOOLEAN_ATTRIBUTE_TYPE, &reachable);
    }

    if (itemChangedMask & Device::kChanged_Name)
    {
        uint8_t zclName[kNodeLabelSize];
        MutableByteSpan zclNameSpan(zclName);
        MakeZclCharString(zclNameSpan, dev->GetName());
        MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID,
                                               ZCL_NODE_LABEL_ATTRIBUTE_ID, ZCL_CHAR_STRING_ATTRIBUTE_TYPE, zclNameSpan.data());
    }

    if (itemChangedMask & Device::kChanged_Location)
    {
        uint8_t buffer[kFixedLabelAttributeArraySize];
        EmberAfAttributeMetadata am = { .attributeId  = ZCL_LABEL_LIST_ATTRIBUTE_ID,
                                        .size         = kFixedLabelAttributeArraySize,
                                        .defaultValue = static_cast<uint32_t>(0) };

        EncodeFixedLabel("room", dev->GetLocation(), buffer, sizeof(buffer), &am);

        MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_FIXED_LABEL_CLUSTER_ID, ZCL_LABEL_LIST_ATTRIBUTE_ID,
                                               ZCL_ARRAY_ATTRIBUTE_TYPE, buffer);
    }
}

void HandleDeviceOnOffStatusChanged(DeviceOnOff * dev, DeviceOnOff::Changed_t itemChangedMask)
{
    if (itemChangedMask & (DeviceOnOff::kChanged_Reachable | DeviceOnOff::kChanged_Name | DeviceOnOff::kChanged_Location))
    {
        HandleDeviceStatusChanged(static_cast<Device *>(dev), (Device::Changed_t) itemChangedMask);
    }

    if (itemChangedMask & DeviceOnOff::kChanged_OnOff)
    {
        uint8_t isOn = dev->IsOn() ? 1 : 0;
        MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_ON_OFF_CLUSTER_ID, ZCL_ON_OFF_ATTRIBUTE_ID,
                                               ZCL_BOOLEAN_ATTRIBUTE_TYPE, &isOn);
    }
}

void HandleDeviceSwitchStatusChanged(DeviceSwitch * dev, DeviceSwitch::Changed_t itemChangedMask)
{
    if (itemChangedMask & (DeviceSwitch::kChanged_Reachable | DeviceSwitch::kChanged_Name | DeviceSwitch::kChanged_Location))
    {
        HandleDeviceStatusChanged(static_cast<Device *>(dev), (Device::Changed_t) itemChangedMask);
    }

    if (itemChangedMask & DeviceSwitch::kChanged_NumberOfPositions)
    {
        uint8_t numberOfPositions = dev->GetNumberOfPositions();
        MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_SWITCH_CLUSTER_ID, ZCL_NUMBER_OF_POSITIONS_ATTRIBUTE_ID,
                                               ZCL_INT8U_ATTRIBUTE_TYPE, &numberOfPositions);
    }

    if (itemChangedMask & DeviceSwitch::kChanged_CurrentPosition)
    {
        uint8_t currentPosition = dev->GetCurrentPosition();
        MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_SWITCH_CLUSTER_ID, ZCL_CURRENT_POSITION_ATTRIBUTE_ID,
                                               ZCL_INT8U_ATTRIBUTE_TYPE, &currentPosition);
    }

    if (itemChangedMask & DeviceSwitch::kChanged_MultiPressMax)
    {
        uint8_t multiPressMax = dev->GetMultiPressMax();
        MatterReportingAttributeChangeCallback(dev->GetEndpointId(), ZCL_SWITCH_CLUSTER_ID, ZCL_MULTI_PRESS_MAX_ATTRIBUTE_ID,
                                               ZCL_INT8U_ATTRIBUTE_TYPE, &multiPressMax);
    }
}

void HandleDevicePowerSourceStatusChanged(DevicePowerSource * dev, DevicePowerSource::Changed_t itemChangedMask)
{
    using namespace app::Clusters;
    if (itemChangedMask &
        (DevicePowerSource::kChanged_Reachable | DevicePowerSource::kChanged_Name | DevicePowerSource::kChanged_Location))
    {
        HandleDeviceStatusChanged(static_cast<Device *>(dev), (Device::Changed_t) itemChangedMask);
    }

    if (itemChangedMask & DevicePowerSource::kChanged_BatLevel)
    {
        uint8_t batChargeLevel = dev->GetBatChargeLevel();
        MatterReportingAttributeChangeCallback(dev->GetEndpointId(), PowerSource::Id,
                                               PowerSource::Attributes::BatteryChargeLevel::Id, ZCL_INT8U_ATTRIBUTE_TYPE,
                                               &batChargeLevel);
    }

    if (itemChangedMask & DevicePowerSource::kChanged_Description)
    {
        MatterReportingAttributeChangeCallback(dev->GetEndpointId(), PowerSource::Id, PowerSource::Attributes::Description::Id);
    }
}

EmberAfStatus HandleReadBridgedDeviceBasicAttribute(Device * dev, chip::AttributeId attributeId, uint8_t * buffer,
                                                    uint16_t maxReadLength)
{
    ChipLogProgress(DeviceLayer, "HandleReadBridgedDeviceBasicAttribute: attrId=%d, maxReadLength=%d", attributeId, maxReadLength);

    if ((attributeId == ZCL_REACHABLE_ATTRIBUTE_ID) && (maxReadLength == 1))
    {
        *buffer = dev->IsReachable() ? 1 : 0;
    }
    else if ((attributeId == ZCL_NODE_LABEL_ATTRIBUTE_ID) && (maxReadLength == 32))
    {
        MutableByteSpan zclNameSpan(buffer, maxReadLength);
        MakeZclCharString(zclNameSpan, dev->GetName());
    }
    else if ((attributeId == ZCL_CLUSTER_REVISION_SERVER_ATTRIBUTE_ID) && (maxReadLength == 2))
    {
        *buffer = (uint16_t) ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_REVISION;
    }
    else
    {
        return EMBER_ZCL_STATUS_FAILURE;
    }

    return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus HandleReadOnOffAttribute(DeviceOnOff * dev, chip::AttributeId attributeId, uint8_t * buffer, uint16_t maxReadLength)
{
    ChipLogProgress(DeviceLayer, "HandleReadOnOffAttribute: attrId=%d, maxReadLength=%d", attributeId, maxReadLength);

    if ((attributeId == ZCL_ON_OFF_ATTRIBUTE_ID) && (maxReadLength == 1))
    {
        *buffer = dev->IsOn() ? 1 : 0;
    }
    else if ((attributeId == ZCL_CLUSTER_REVISION_SERVER_ATTRIBUTE_ID) && (maxReadLength == 2))
    {
        *buffer = (uint16_t) ZCL_ON_OFF_CLUSTER_REVISION;
    }
    else
    {
        return EMBER_ZCL_STATUS_FAILURE;
    }

    return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus HandleWriteOnOffAttribute(DeviceOnOff * dev, chip::AttributeId attributeId, uint8_t * buffer)
{
    ChipLogProgress(DeviceLayer, "HandleWriteOnOffAttribute: attrId=%d", attributeId);

    if ((attributeId == ZCL_ON_OFF_ATTRIBUTE_ID) && (dev->IsReachable()))
    {
        if (*buffer)
        {
            dev->SetOnOff(true);
        }
        else
        {
            dev->SetOnOff(false);
        }
    }
    else
    {
        return EMBER_ZCL_STATUS_FAILURE;
    }

    return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus HandleReadFixedLabelAttribute(Device * dev, const EmberAfAttributeMetadata * am, uint8_t * buffer,
                                            uint16_t maxReadLength)
{
    if ((am->attributeId == ZCL_LABEL_LIST_ATTRIBUTE_ID) && (maxReadLength <= kFixedLabelAttributeArraySize))
    {
        EncodeFixedLabel("room", dev->GetLocation(), buffer, maxReadLength, am);
    }
    else if ((am->attributeId == ZCL_CLUSTER_REVISION_SERVER_ATTRIBUTE_ID) && (maxReadLength == 2))
    {
        *buffer = (uint16_t) ZCL_FIXED_LABEL_CLUSTER_REVISION;
    }
    else
    {
        return EMBER_ZCL_STATUS_FAILURE;
    }

    return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus HandleReadSwitchAttribute(DeviceSwitch * dev, chip::AttributeId attributeId, uint8_t * buffer, uint16_t maxReadLength)
{
    if ((attributeId == ZCL_NUMBER_OF_POSITIONS_ATTRIBUTE_ID) && (maxReadLength == 1))
    {
        *buffer = dev->GetNumberOfPositions();
    }
    else if ((attributeId == ZCL_CURRENT_POSITION_ATTRIBUTE_ID) && (maxReadLength == 1))
    {
        *buffer = dev->GetCurrentPosition();
    }
    else if ((attributeId == ZCL_MULTI_PRESS_MAX_ATTRIBUTE_ID) && (maxReadLength == 1))
    {
        *buffer = dev->GetMultiPressMax();
    }
    else if ((attributeId == ZCL_FEATURE_MAP_SERVER_ATTRIBUTE_ID) && (maxReadLength == 4))
    {
        *(uint32_t *) buffer = dev->GetFeatureMap();
    }
    else if ((attributeId == ZCL_CLUSTER_REVISION_SERVER_ATTRIBUTE_ID) && (maxReadLength == 2))
    {
        *buffer = (uint16_t) ZCL_SWITCH_CLUSTER_REVISION;
    }
    else
    {
        return EMBER_ZCL_STATUS_FAILURE;
    }

    return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus HandleReadPowerSourceAttribute(DevicePowerSource * dev, chip::AttributeId attributeId, uint8_t * buffer,
                                             uint16_t maxReadLength)
{
    using namespace app::Clusters;
    if ((attributeId == PowerSource::Attributes::BatteryChargeLevel::Id) && (maxReadLength == 1))
    {
        *buffer = dev->GetBatChargeLevel();
    }
    else if ((attributeId == PowerSource::Attributes::Order::Id) && (maxReadLength == 1))
    {
        *buffer = dev->GetOrder();
    }
    else if ((attributeId == PowerSource::Attributes::Status::Id) && (maxReadLength == 1))
    {
        *buffer = dev->GetStatus();
    }
    else if ((attributeId == PowerSource::Attributes::Description::Id) && (maxReadLength == 32))
    {
        MutableByteSpan zclDescpitionSpan(buffer, maxReadLength);
        MakeZclCharString(zclDescpitionSpan, dev->GetDescription().c_str());
    }
    else if ((attributeId == PowerSource::Attributes::ClusterRevision::Id) && (maxReadLength == 2))
    {
        uint16_t rev = ZCL_POWER_SOURCE_CLUSTER_REVISION;
        memcpy(buffer, &rev, sizeof(rev));
    }
    else if ((attributeId == PowerSource::Attributes::FeatureMap::Id) && (maxReadLength == 4))
    {
        uint32_t featureMap = dev->GetFeatureMap();
        memcpy(buffer, &featureMap, sizeof(featureMap));
    }
    else
    {
        return EMBER_ZCL_STATUS_FAILURE;
    }

    return EMBER_ZCL_STATUS_SUCCESS;
}

EmberAfStatus emberAfExternalAttributeReadCallback(EndpointId endpoint, ClusterId clusterId,
                                                   const EmberAfAttributeMetadata * attributeMetadata, uint8_t * buffer,
                                                   uint16_t maxReadLength)
{
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);

    EmberAfStatus ret = EMBER_ZCL_STATUS_FAILURE;

    if ((endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT) && (gDevices[endpointIndex] != nullptr))
    {
        Device * dev = gDevices[endpointIndex];

        if (clusterId == ZCL_BRIDGED_DEVICE_BASIC_CLUSTER_ID)
        {
            ret = HandleReadBridgedDeviceBasicAttribute(dev, attributeMetadata->attributeId, buffer, maxReadLength);
        }
        else if (clusterId == ZCL_FIXED_LABEL_CLUSTER_ID)
        {
            ret = HandleReadFixedLabelAttribute(dev, attributeMetadata, buffer, maxReadLength);
        }
        else if (clusterId == ZCL_ON_OFF_CLUSTER_ID)
        {
            ret = HandleReadOnOffAttribute(static_cast<DeviceOnOff *>(dev), attributeMetadata->attributeId, buffer, maxReadLength);
        }
        else if (clusterId == ZCL_SWITCH_CLUSTER_ID)
        {
            ret =
                HandleReadSwitchAttribute(static_cast<DeviceSwitch *>(dev), attributeMetadata->attributeId, buffer, maxReadLength);
        }
        else if (clusterId == chip::app::Clusters::PowerSource::Id)
        {
            ret = HandleReadPowerSourceAttribute(static_cast<DevicePowerSource *>(dev), attributeMetadata->attributeId, buffer,
                                                 maxReadLength);
        }
    }

    return ret;
}

EmberAfStatus emberAfExternalAttributeWriteCallback(EndpointId endpoint, ClusterId clusterId,
                                                    const EmberAfAttributeMetadata * attributeMetadata, uint8_t * buffer)
{
    uint16_t endpointIndex = emberAfGetDynamicIndexFromEndpoint(endpoint);

    EmberAfStatus ret = EMBER_ZCL_STATUS_FAILURE;

    // ChipLogProgress(DeviceLayer, "emberAfExternalAttributeWriteCallback: ep=%d", endpoint);

    if (endpointIndex < CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT)
    {
        Device * dev = gDevices[endpointIndex];

        if ((dev->IsReachable()) && (clusterId == ZCL_ON_OFF_CLUSTER_ID))
        {
            ret = HandleWriteOnOffAttribute(static_cast<DeviceOnOff *>(dev), attributeMetadata->attributeId, buffer);
        }
    }

    return ret;
}

void ApplicationInit() {}

const EmberAfDeviceType gBridgedRootDeviceTypes[] = { { DEVICE_TYPE_ROOT_NODE, DEVICE_VERSION_DEFAULT },
                                                      { DEVICE_TYPE_BRIDGE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedOnOffDeviceTypes[] = { { DEVICE_TYPE_LO_ON_OFF_LIGHT, DEVICE_VERSION_DEFAULT },
                                                       { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedSwitchDeviceTypes[] = { { DEVICE_TYPE_LO_ON_OFF_LIGHT_SWITCH, DEVICE_VERSION_DEFAULT },
                                                        { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gBridgedComposedDeviceTypes[] = { { DEVICE_TYPE_BRIDGED_NODE, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gComposedSwitchDeviceTypes[] = { { DEVICE_TYPE_LO_ON_OFF_LIGHT_SWITCH, DEVICE_VERSION_DEFAULT } };

const EmberAfDeviceType gComposedPowerSourceDeviceTypes[] = { { DEVICE_TYPE_POWER_SOURCE, DEVICE_VERSION_DEFAULT } };

int main(int argc, char * argv[])
{
    // Clear out the device database
    memset(gDevices, 0, sizeof(gDevices));

    // Create Mock Devices

    // Define 4 lights
    DeviceOnOff Light1("Light 1", "Office");
    DeviceOnOff Light2("Light 2", "Office");
    DeviceOnOff Light3("Light 3", "Office");
    DeviceOnOff Light4("Light 4", "Den");

    Light1.SetChangeCallback(&HandleDeviceOnOffStatusChanged);
    Light2.SetChangeCallback(&HandleDeviceOnOffStatusChanged);
    Light3.SetChangeCallback(&HandleDeviceOnOffStatusChanged);
    Light4.SetChangeCallback(&HandleDeviceOnOffStatusChanged);

    Light1.SetReachable(true);
    Light2.SetReachable(true);
    Light3.SetReachable(true);
    Light4.SetReachable(true);

    // Define 2 switches
    DeviceSwitch Switch1("Switch 1", "Office", EMBER_AF_SWITCH_FEATURE_LATCHING_SWITCH);
    DeviceSwitch Switch2("Switch 2", "Office",
                         EMBER_AF_SWITCH_FEATURE_MOMENTARY_SWITCH | EMBER_AF_SWITCH_FEATURE_MOMENTARY_SWITCH_RELEASE |
                             EMBER_AF_SWITCH_FEATURE_MOMENTARY_SWITCH_LONG_PRESS |
                             EMBER_AF_SWITCH_FEATURE_MOMENTARY_SWITCH_MULTI_PRESS);

    Switch1.SetChangeCallback(&HandleDeviceSwitchStatusChanged);
    Switch2.SetChangeCallback(&HandleDeviceSwitchStatusChanged);

    Switch1.SetReachable(true);
    Switch2.SetReachable(true);

    // Define composed device with two switches
    ComposedDevice ComposedDevice("Composed Switcher", "Bedroom");
    DeviceSwitch ComposedSwitch1("Composed Switch 1", "Bedroom", EMBER_AF_SWITCH_FEATURE_LATCHING_SWITCH);
    DeviceSwitch ComposedSwitch2("Composed Switch 2", "Bedroom",
                                 EMBER_AF_SWITCH_FEATURE_MOMENTARY_SWITCH | EMBER_AF_SWITCH_FEATURE_MOMENTARY_SWITCH_RELEASE |
                                     EMBER_AF_SWITCH_FEATURE_MOMENTARY_SWITCH_LONG_PRESS |
                                     EMBER_AF_SWITCH_FEATURE_MOMENTARY_SWITCH_MULTI_PRESS);
    DevicePowerSource ComposedPowerSource("Composed Power Source", "Bedroom", EMBER_AF_POWER_SOURCE_FEATURE_BATTERY);

    ComposedSwitch1.SetChangeCallback(&HandleDeviceSwitchStatusChanged);
    ComposedSwitch2.SetChangeCallback(&HandleDeviceSwitchStatusChanged);
    ComposedPowerSource.SetChangeCallback(&HandleDevicePowerSourceStatusChanged);

    ComposedDevice.SetReachable(true);
    ComposedSwitch1.SetReachable(true);
    ComposedSwitch2.SetReachable(true);
    ComposedPowerSource.SetReachable(true);
    ComposedPowerSource.SetBatChargeLevel(58);

    if (ChipLinuxAppInit(argc, argv) != 0)
    {
        return -1;
    }

    // Init Data Model and CHIP App Server
    static chip::CommonCaseDeviceServerInitParams initParams;
    (void) initParams.InitializeStaticResourcesBeforeServerInit();

#if CHIP_DEVICE_ENABLE_PORT_PARAMS
    // use a different service port to make testing possible with other sample devices running on same host
    initParams.operationalServicePort = LinuxDeviceOptions::GetInstance().securedDevicePort;
#endif

    initParams.interfaceId = LinuxDeviceOptions::GetInstance().interfaceId;
    chip::Server::GetInstance().Init(initParams);

    // Initialize device attestation config
    SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());

    // Set starting endpoint id where dynamic endpoints will be assigned, which
    // will be the next consecutive endpoint id after the last fixed endpoint.
    gFirstDynamicEndpointId = static_cast<chip::EndpointId>(
        static_cast<int>(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1))) + 1);
    gCurrentEndpointId = gFirstDynamicEndpointId;

    // Disable last fixed endpoint, which is used as a placeholder for all of the
    // supported clusters so that ZAP will generated the requisite code.
    emberAfEndpointEnableDisable(emberAfEndpointFromIndex(static_cast<uint16_t>(emberAfFixedEndpointCount() - 1)), false);

    //
    // By default, ZAP only supports specifying a single device type in the UI. However for bridges, they are both
    // a Bridge and Matter Root Node device on EP0. Consequently, over-ride the generated value to correct this.
    //
    emberAfSetDeviceTypeList(0, Span<const EmberAfDeviceType>(gBridgedRootDeviceTypes));

    // Add lights 1..3 --> will be mapped to ZCL endpoints 2, 3, 4
    AddDeviceEndpoint(&Light1, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
                      Span<DataVersion>(gLight1DataVersions));
    AddDeviceEndpoint(&Light2, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
                      Span<DataVersion>(gLight2DataVersions));
    AddDeviceEndpoint(&Light3, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
                      Span<DataVersion>(gLight3DataVersions));

    // Remove Light 2 -- Lights 1 & 3 will remain mapped to endpoints 2 & 4
    RemoveDeviceEndpoint(&Light2);

    // Add Light 4 -- > will be mapped to ZCL endpoint 5
    AddDeviceEndpoint(&Light4, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
                      Span<DataVersion>(gLight4DataVersions));

    // Re-add Light 2 -- > will be mapped to ZCL endpoint 6
    AddDeviceEndpoint(&Light2, &bridgedLightEndpoint, Span<const EmberAfDeviceType>(gBridgedOnOffDeviceTypes),
                      Span<DataVersion>(gLight2DataVersions));

    // Add switch 1..2 --> will be mapped to ZCL endpoints 7,8
    AddDeviceEndpoint(&Switch1, &bridgedSwitchEndpoint, Span<const EmberAfDeviceType>(gBridgedSwitchDeviceTypes),
                      Span<DataVersion>(gSwitch1DataVersions));
    AddDeviceEndpoint(&Switch2, &bridgedSwitchEndpoint, Span<const EmberAfDeviceType>(gBridgedSwitchDeviceTypes),
                      Span<DataVersion>(gSwitch2DataVersions));

    // Add composed Device with two buttons and a power source
    AddDeviceEndpoint(&ComposedDevice, &bridgedComposedDeviceEndpoint, Span<const EmberAfDeviceType>(gBridgedComposedDeviceTypes),
                      Span<DataVersion>(gComposedDeviceDataVersions));
    AddDeviceEndpoint(&ComposedSwitch1, &bridgedSwitchEndpoint, Span<const EmberAfDeviceType>(gComposedSwitchDeviceTypes),
                      Span<DataVersion>(gComposedSwitch1DataVersions), ComposedDevice.GetEndpointId());
    AddDeviceEndpoint(&ComposedSwitch2, &bridgedSwitchEndpoint, Span<const EmberAfDeviceType>(gComposedSwitchDeviceTypes),
                      Span<DataVersion>(gComposedSwitch2DataVersions), ComposedDevice.GetEndpointId());
    AddDeviceEndpoint(&ComposedPowerSource, &bridgedPowerSourceEndpoint,
                      Span<const EmberAfDeviceType>(gComposedPowerSourceDeviceTypes),
                      Span<DataVersion>(gComposedPowerSourceDataVersions), ComposedDevice.GetEndpointId());

    // Run CHIP

    chip::DeviceLayer::PlatformMgr().RunEventLoop();

    return 0;
}
