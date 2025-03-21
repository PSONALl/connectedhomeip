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

/**
 *    @file
 *      This file implements the ExchangeContext class.
 *
 */
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

#include <lib/core/CHIPCore.h>
#include <lib/core/CHIPEncoding.h>
#include <lib/core/CHIPKeyIds.h>
#include <lib/support/Defer.h>
#include <lib/support/TypeTraits.h>
#include <lib/support/logging/CHIPLogging.h>
#include <messaging/ApplicationExchangeDispatch.h>
#include <messaging/ExchangeContext.h>
#include <messaging/ExchangeMgr.h>
#include <protocols/Protocols.h>
#include <protocols/secure_channel/Constants.h>

#if CONFIG_DEVICE_LAYER
#include <platform/CHIPDeviceLayer.h>
#endif

using namespace chip::Encoding;
using namespace chip::Inet;
using namespace chip::System;

namespace chip {
namespace Messaging {

static void DefaultOnMessageReceived(ExchangeContext * ec, Protocols::Id protocolId, uint8_t msgType, uint32_t messageCounter,
                                     PacketBufferHandle && payload)
{
    ChipLogError(ExchangeManager,
                 "Dropping unexpected message of type " ChipLogFormatMessageType " with protocolId " ChipLogFormatProtocolId
                 " and MessageCounter:" ChipLogFormatMessageCounter " on exchange " ChipLogFormatExchange,
                 msgType, ChipLogValueProtocolId(protocolId), messageCounter, ChipLogValueExchange(ec));
}

bool ExchangeContext::IsInitiator() const
{
    return mFlags.Has(Flags::kFlagInitiator);
}

bool ExchangeContext::IsResponseExpected() const
{
    return mFlags.Has(Flags::kFlagResponseExpected);
}

void ExchangeContext::SetResponseExpected(bool inResponseExpected)
{
    mFlags.Set(Flags::kFlagResponseExpected, inResponseExpected);
}

void ExchangeContext::SetResponseTimeout(Timeout timeout)
{
    mResponseTimeout = timeout;
}

#if CONFIG_DEVICE_LAYER && CHIP_DEVICE_CONFIG_ENABLE_SED
void ExchangeContext::UpdateSEDIntervalMode()
{
    if (!HasSessionHandle())
    {
        // After the session has been deleted, no further communication can occur on the exchange,
        // so withdraw a SED active mode request.
        UpdateSEDIntervalMode(false);
        return;
    }

    Transport::PeerAddress address;

    switch (GetSessionHandle()->GetSessionType())
    {
    case Transport::Session::SessionType::kSecure:
        address = GetSessionHandle()->AsSecureSession()->GetPeerAddress();
        break;
    case Transport::Session::SessionType::kUnauthenticated:
        address = GetSessionHandle()->AsUnauthenticatedSession()->GetPeerAddress();
        break;
    default:
        return;
    }

    VerifyOrReturn(address.GetTransportType() != Transport::Type::kBle);
    UpdateSEDIntervalMode(IsResponseExpected() || IsSendExpected() || IsMessageNotAcked());
}

void ExchangeContext::UpdateSEDIntervalMode(bool activeMode)
{
    if (activeMode != IsRequestingActiveMode())
    {
        SetRequestingActiveMode(activeMode);
        DeviceLayer::ConnectivityMgr().RequestSEDActiveMode(activeMode);
    }
}
#endif

CHIP_ERROR ExchangeContext::SendMessage(Protocols::Id protocolId, uint8_t msgType, PacketBufferHandle && msgBuf,
                                        const SendFlags & sendFlags)
{
    bool isStandaloneAck =
        (protocolId == Protocols::SecureChannel::Id) && msgType == to_underlying(Protocols::SecureChannel::MsgType::StandaloneAck);
    if (!isStandaloneAck)
    {
        // If we were waiting for a message send, this is it.  Standalone acks
        // are not application-level sends, which is why we don't allow those to
        // clear the WillSendMessage flag.
        mFlags.Clear(Flags::kFlagWillSendMessage);
    }

    VerifyOrReturnError(mExchangeMgr != nullptr, CHIP_ERROR_INTERNAL);
    VerifyOrReturnError(mSession, CHIP_ERROR_CONNECTION_ABORTED);

    // Don't let method get called on a freed object.
    VerifyOrDie(mExchangeMgr != nullptr && GetReferenceCount() > 0);

    // we hold the exchange context here in case the entity that
    // originally generated it tries to close it as a result of
    // an error arising below. at the end, we have to close it.
    ExchangeHandle ref(*this);

    // If session requires MRP, NoAutoRequestAck send flag is not specified and is not a group exchange context, request reliable
    // transmission.
    bool reliableTransmissionRequested =
        GetSessionHandle()->RequireMRP() && !sendFlags.Has(SendMessageFlags::kNoAutoRequestAck) && !IsGroupExchangeContext();

    // If a response message is expected...
    if (sendFlags.Has(SendMessageFlags::kExpectResponse) && !IsGroupExchangeContext())
    {
        // Only one 'response expected' message can be outstanding at a time.
        if (IsResponseExpected())
        {
            // TODO: add a test for this case.
            return CHIP_ERROR_INCORRECT_STATE;
        }

        SetResponseExpected(true);

        // Arm the response timer if a timeout has been specified.
        if (mResponseTimeout > System::Clock::kZero)
        {
            CHIP_ERROR err = StartResponseTimer();
            if (err != CHIP_NO_ERROR)
            {
                SetResponseExpected(false);
                return err;
            }
        }
    }

    {
        // ExchangeContext for group are supposed to always be Initiator
        if (IsGroupExchangeContext() && !IsInitiator())
        {
            return CHIP_ERROR_INTERNAL;
        }

        //
        // It is possible that we might have evicted a session as a side-effect of processing an inbound message on this exchange.
        // We cannot proceed any further sending a message since we don't have an attached session, so let's error out.
        //
        // This should not happen to well-behaved logic attempting to sending messages on exchanges, so let's print out a warning
        // to ensure it alerts someone to fixing their logic...
        //
        if (!mSession)
        {
            ChipLogError(ExchangeManager,
                         "WARNING: We shouldn't be sending a message on an exchange that has no attached session...");
            return CHIP_ERROR_MISSING_SECURE_SESSION;
        }

        // Create a new scope for `err`, to avoid shadowing warning previous `err`.
        CHIP_ERROR err = mDispatch.SendMessage(GetExchangeMgr()->GetSessionManager(), mSession.Get().Value(), mExchangeId,
                                               IsInitiator(), GetReliableMessageContext(), reliableTransmissionRequested,
                                               protocolId, msgType, std::move(msgBuf));
        if (err != CHIP_NO_ERROR && IsResponseExpected())
        {
            CancelResponseTimer();
            SetResponseExpected(false);
        }

        // Standalone acks are not application-level message sends.
        if (err == CHIP_NO_ERROR && !isStandaloneAck)
        {
            MessageHandled();
        }

        return err;
    }
}

void ExchangeContext::DoClose(bool clearRetransTable)
{
    mFlags.Set(Flags::kFlagClosed);

    // Clear protocol callbacks
    if (mDelegate != nullptr)
    {
        mDelegate->OnExchangeClosing(this);
    }
    mDelegate = nullptr;

    // Closure of an exchange context is based on ref counting. The Protocol, when it calls DoClose(), indicates that
    // it is done with the exchange context and the message layer sets all callbacks to NULL and does not send anything
    // received on the exchange context up to higher layers.  At this point, the message layer needs to handle the
    // remaining work to be done on that exchange, (e.g. send all pending acks) before truly cleaning it up.
    FlushAcks();

    // In case the protocol wants a harder release of the EC right away, such as calling Abort(), exchange
    // needs to clear the MRP retransmission table immediately.
    if (clearRetransTable)
    {
        mExchangeMgr->GetReliableMessageMgr()->ClearRetransTable(this);
    }

    // Cancel the response timer.
    CancelResponseTimer();
}

/**
 *  Gracefully close an exchange context. This call decrements the
 *  reference count and releases the exchange when the reference
 *  count goes to zero.
 *
 */
void ExchangeContext::Close()
{
    VerifyOrDie(mExchangeMgr != nullptr && GetReferenceCount() > 0);

#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
    ChipLogDetail(ExchangeManager, "ec - close[" ChipLogFormatExchange "], %s", ChipLogValueExchange(this), __func__);
#endif

    DoClose(false);
    Release();
}
/**
 *  Abort the Exchange context immediately and release all
 *  references to it.
 *
 */
void ExchangeContext::Abort()
{
    VerifyOrDie(mExchangeMgr != nullptr && GetReferenceCount() > 0);

#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
    ChipLogDetail(ExchangeManager, "ec - abort[" ChipLogFormatExchange "], %s", ChipLogValueExchange(this), __func__);
#endif

    DoClose(true);
    Release();
}

void ExchangeContextDeletor::Release(ExchangeContext * ec)
{
    ec->mExchangeMgr->ReleaseContext(ec);
}

ExchangeContext::ExchangeContext(ExchangeManager * em, uint16_t ExchangeId, const SessionHandle & session, bool Initiator,
                                 ExchangeDelegate * delegate) :
    mDispatch((delegate != nullptr) ? delegate->GetMessageDispatch() : ApplicationExchangeDispatch::Instance()),
    mSession(*this)
{
    VerifyOrDie(mExchangeMgr == nullptr);

    mExchangeMgr = em;
    mExchangeId  = ExchangeId;
    mSession.Grab(session);
    mFlags.Set(Flags::kFlagInitiator, Initiator);
    mDelegate = delegate;

    SetDropAckDebug(false);
    SetAckPending(false);

    // Do not request Ack for multicast
    SetAutoRequestAck(!session->IsGroupSession());

#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
    ChipLogDetail(ExchangeManager, "ec++ id: " ChipLogFormatExchange, ChipLogValueExchange(this));
#endif
    SYSTEM_STATS_INCREMENT(chip::System::Stats::kExchangeMgr_NumContexts);
}

ExchangeContext::~ExchangeContext()
{
    VerifyOrDie(mExchangeMgr != nullptr && GetReferenceCount() == 0);
    VerifyOrDie(!IsAckPending());

#if CONFIG_DEVICE_LAYER && CHIP_DEVICE_CONFIG_ENABLE_SED
    // Make sure that the exchange withdraws the request for Sleepy End Device active mode.
    UpdateSEDIntervalMode(false);
#endif

    // Ideally, in this scenario, the retransmit table should
    // be clear of any outstanding messages for this context and
    // the boolean parameter passed to DoClose() should not matter.

    DoClose(false);
    mExchangeMgr = nullptr;

#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
    ChipLogDetail(ExchangeManager, "ec-- id: " ChipLogFormatExchange, ChipLogValueExchange(this));
#endif
    SYSTEM_STATS_DECREMENT(chip::System::Stats::kExchangeMgr_NumContexts);
}

bool ExchangeContext::MatchExchange(const SessionHandle & session, const PacketHeader & packetHeader,
                                    const PayloadHeader & payloadHeader)
{
    // A given message is part of a particular exchange if...
    return

        // The exchange identifier of the message matches the exchange identifier of the context.
        (mExchangeId == payloadHeader.GetExchangeID())

        // AND The Session associated with the incoming message matches the Session associated with the exchange.
        && (mSession.Contains(session))

        // TODO: This check should be already implied by the equality of session check,
        // It should be removed after we have implemented the temporary node id for PASE and CASE sessions
        && (IsEncryptionRequired() == packetHeader.IsEncrypted())

        // AND The message was sent by an initiator and the exchange context is a responder (IsInitiator==false)
        //    OR The message was sent by a responder and the exchange context is an initiator (IsInitiator==true) (for the broadcast
        //    case, the initiator is ill defined)

        && (payloadHeader.IsInitiator() != IsInitiator());
}

void ExchangeContext::OnSessionReleased()
{
    if (mFlags.Has(Flags::kFlagClosed))
    {
        // Exchange is already being closed. It may occur when closing an exchange after sending
        // RemoveFabric response which triggers removal of all sessions for the given fabric.
        mExchangeMgr->GetReliableMessageMgr()->ClearRetransTable(this);
        return;
    }

    ExchangeHandle ref(*this);

    if (IsResponseExpected())
    {
        // If we're waiting on a response, we now know it's never going to show up
        // and we should notify our delegate accordingly.
        CancelResponseTimer();
        SetResponseExpected(false);
        NotifyResponseTimeout();
    }

    DoClose(true /* clearRetransTable */);
}

CHIP_ERROR ExchangeContext::StartResponseTimer()
{
    System::Layer * lSystemLayer = mExchangeMgr->GetSessionManager()->SystemLayer();
    if (lSystemLayer == nullptr)
    {
        // this is an assertion error, which shall never happen
        return CHIP_ERROR_INTERNAL;
    }

    return lSystemLayer->StartTimer(mResponseTimeout, HandleResponseTimeout, this);
}

void ExchangeContext::CancelResponseTimer()
{
    System::Layer * lSystemLayer = mExchangeMgr->GetSessionManager()->SystemLayer();
    if (lSystemLayer == nullptr)
    {
        // this is an assertion error, which shall never happen
        return;
    }

    lSystemLayer->CancelTimer(HandleResponseTimeout, this);
}

void ExchangeContext::HandleResponseTimeout(System::Layer * aSystemLayer, void * aAppState)
{
    ExchangeContext * ec = reinterpret_cast<ExchangeContext *>(aAppState);

    if (ec == nullptr)
        return;

    ec->NotifyResponseTimeout();
}

void ExchangeContext::NotifyResponseTimeout()
{
    SetResponseExpected(false);

    ExchangeDelegate * delegate = GetDelegate();

    // Call the user's timeout handler.
    if (delegate != nullptr)
    {
        delegate->OnResponseTimeout(this);
    }

    MessageHandled();
}

CHIP_ERROR ExchangeContext::HandleMessage(uint32_t messageCounter, const PayloadHeader & payloadHeader, MessageFlags msgFlags,
                                          PacketBufferHandle && msgBuf)
{
    // We hold a reference to the ExchangeContext here to
    // guard against Close() calls(decrementing the reference
    // count) by the protocol before the CHIP Exchange
    // layer has completed its work on the ExchangeContext.
    ExchangeHandle ref(*this);

    bool isStandaloneAck = payloadHeader.HasMessageType(Protocols::SecureChannel::MsgType::StandaloneAck);
    bool isDuplicate     = msgFlags.Has(MessageFlagValues::kDuplicateMessage);

    auto deferred = MakeDefer([&]() {
        // Duplicates and standalone acks are not application-level messages, so they should generally not lead to any state
        // changes.  The one exception to that is that if we have a null mDelegate then our lifetime is not application-defined,
        // since we don't interact with the application at that point.  That can happen when we are already closed (in which case
        // MessageHandled is a no-op) or if we were just created to send a standalone ack for this incoming message, in which case
        // we should treat it as an app-level message for purposes of our state.
        if ((isStandaloneAck || isDuplicate) && mDelegate != nullptr)
        {
            return;
        }

        MessageHandled();
    });

    ReturnErrorOnFailure(mDispatch.OnMessageReceived(messageCounter, payloadHeader, msgFlags, GetReliableMessageContext()));

    if (IsAckPending() && !mDelegate)
    {
        // The incoming message wants an ack, but we have no delegate, so
        // there's not going to be a response to piggyback on.  Just flush the
        // ack out right now.
        ReturnErrorOnFailure(FlushAcks());
    }

    // The SecureChannel::StandaloneAck message type is only used for MRP; do not pass such messages to the application layer.
    if (isStandaloneAck)
    {
        return CHIP_NO_ERROR;
    }

    // Since the message is duplicate, let's not forward it up the stack
    if (isDuplicate)
    {
        return CHIP_NO_ERROR;
    }

    // Since we got the response, cancel the response timer.
    CancelResponseTimer();

    // If the context was expecting a response to a previously sent message, this message
    // is implicitly that response.
    SetResponseExpected(false);

    if (mDelegate != nullptr)
    {
        return mDelegate->OnMessageReceived(this, payloadHeader, std::move(msgBuf));
    }

    DefaultOnMessageReceived(this, payloadHeader.GetProtocolID(), payloadHeader.GetMessageType(), messageCounter,
                             std::move(msgBuf));
    return CHIP_NO_ERROR;
}

void ExchangeContext::MessageHandled()
{
#if CONFIG_DEVICE_LAYER && CHIP_DEVICE_CONFIG_ENABLE_SED
    UpdateSEDIntervalMode();
#endif

    if (mFlags.Has(Flags::kFlagClosed) || IsResponseExpected() || IsSendExpected())
    {
        return;
    }

    Close();
}

} // namespace Messaging
} // namespace chip
