﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "pch.h"

#include "PushNotificationManager.h"
#include "Microsoft.Windows.PushNotifications.PushNotificationManager.g.cpp"
#include "PushNotificationTelemetry.h"
#include "PushNotificationCreateChannelResult.h"
#include "PushNotifications-Constants.h"
#include <winrt/Windows.ApplicationModel.background.h>
#include <winrt/Windows.Networking.PushNotifications.h>
#include "PushNotificationBackgroundTask.h"
#include <winerror.h>
#include <algorithm>
#include "PushNotificationChannel.h"
#include "externs.h"
#include <string_view>
#include <frameworkudk/pushnotifications.h>
#include "NotificationsLongRunningProcess_h.h"
#include "PushNotificationUtility.h"
#include "AppNotificationUtility.h"
#include "PushNotificationReceivedEventArgs.h"

using namespace std::literals;
using namespace Microsoft::Windows::AppNotifications::Helpers;

constexpr std::wstring_view backgroundTaskName = L"PushBackgroundTaskName"sv;

static wil::unique_event g_waitHandleForArgs;
static winrt::guid g_comServerClsid{ GUID_NULL };

wil::unique_event& GetWaitHandleForArgs()
{
    return g_waitHandleForArgs;
}

winrt::guid& GetComServerClsid()
{
    return g_comServerClsid;
}

namespace winrt
{
    using namespace Windows::ApplicationModel::Background;
    using namespace Windows::Networking::PushNotifications;
    using namespace Windows::Foundation;
}

namespace PushNotificationHelpers
{
    using namespace winrt::Microsoft::Windows::PushNotifications::Helpers;
}
namespace winrt::Microsoft::Windows::PushNotifications::implementation
{
    inline constexpr auto c_maxBackoff{ 5min };
    inline constexpr auto c_initialBackoff{ 60s };
    inline constexpr auto c_backoffIncrement{ 60s };
     
    const HRESULT WNP_E_NOT_CONNECTED = static_cast<HRESULT>(0x880403E8L);
    const HRESULT WNP_E_RECONNECTING = static_cast<HRESULT>(0x880403E9L);
    const HRESULT WNP_E_BIND_USER_BUSY = static_cast<HRESULT>(0x880403FEL);

    bool IsChannelRequestRetryable(const hresult& hr)
    {
        switch (hr)
        {
        case HRESULT_FROM_WIN32(ERROR_TIMEOUT):
        case WNP_E_NOT_CONNECTED:
        case WPN_E_OUTSTANDING_CHANNEL_REQUEST:
        case WNP_E_RECONNECTING:
        case WNP_E_BIND_USER_BUSY:
        case HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE):
            return true;
        default:
            return false;
        }
    }

    void RegisterSinkHelper()
    {
        if (PushNotificationHelpers::IsPackagedAppScenario())
        {
            auto appUserModelId{ PushNotificationHelpers::GetAppUserModelId() };

            // Register a sink with platform which is initialized in the current process
            THROW_IF_FAILED(PushNotifications_RegisterNotificationSinkForFullTrustApplication(appUserModelId.get(), PushNotificationManager::Default().as<ABI::Microsoft::Internal::PushNotifications::INotificationListener>().get()));
        }
        else
        {
            auto notificationsLongRunningPlatform{ PushNotificationHelpers::GetNotificationPlatform() };

            wil::unique_cotaskmem_string processName;
            THROW_IF_FAILED(GetCurrentProcessPath(processName));

            // Register a sink with platform brokered through PushNotificationsLongRunningProcess
            THROW_IF_FAILED(notificationsLongRunningPlatform->RegisterForegroundActivator(PushNotificationManager::Default().as<IWpnForegroundSink>().get(), processName.get()));
        }
    }

    void RegisterUnpackagedApplicationHelper(
        const winrt::guid& remoteId,
        _Out_ wil::unique_cotaskmem_string &unpackagedAppUserModelId)
    {
        auto coInitialize = wil::CoInitializeEx();

        auto notificationPlatform{ PushNotificationHelpers::GetNotificationPlatform() };

        wil::unique_cotaskmem_string processName;
        THROW_IF_FAILED(GetCurrentProcessPath(processName));

        THROW_IF_FAILED(notificationPlatform->RegisterFullTrustApplication(processName.get(), remoteId, &unpackagedAppUserModelId));
    }

    winrt::hresult CreateChannelWithRemoteIdHelper(wil::unique_cotaskmem_string const& appId, const winrt::guid& remoteId, ChannelDetails& channelInfo) noexcept try
    {
        HRESULT operationalCode{};
        ABI::Windows::Foundation::DateTime channelExpiryTime{};
        wil::unique_cotaskmem_string channelId;
        wil::unique_cotaskmem_string channelUri;

        THROW_IF_FAILED(PushNotifications_CreateChannelWithRemoteIdentifier(
            appId.get(),
            remoteId,
            &operationalCode,
            &channelId,
            &channelUri,
            &channelExpiryTime));

        THROW_IF_FAILED(operationalCode);

        winrt::copy_from_abi(channelInfo.channelExpiryTime, &channelExpiryTime);
        channelInfo.appId = winrt::hstring{ appId.get() };
        channelInfo.channelId = winrt::hstring{ channelId.get() };
        channelInfo.channelUri = winrt::hstring{ channelUri.get() };

        return S_OK;
    }
    CATCH_RETURN()

    winrt::Microsoft::Windows::PushNotifications::PushNotificationManager PushNotificationManager::Default()
    {
        static auto pushNotificationManager{ winrt::make<PushNotificationManager>() };
        return pushNotificationManager;
    }

    winrt::IAsyncOperationWithProgress<winrt::Microsoft::Windows::PushNotifications::PushNotificationCreateChannelResult, winrt::Microsoft::Windows::PushNotifications::PushNotificationCreateChannelStatus> PushNotificationManager::CreateChannelAsync(const winrt::guid remoteId)
    {
        THROW_HR_IF(E_NOTIMPL, !::Microsoft::Windows::PushNotifications::Feature_PushNotifications::IsEnabled());

        wil::winrt_module_reference moduleRef{};

        try
        {
            THROW_HR_IF(E_INVALIDARG, (remoteId == winrt::guid()));

            auto cancellation{ co_await winrt::get_cancellation_token() };

            cancellation.enable_propagation(true);

            // Allow to register the progress and complete handler
            co_await resume_background();

            auto progress{ co_await winrt::get_progress_token() };

            uint8_t retryCount = 0;
            winrt::hresult channelRequestResult = E_PENDING;
            PushNotificationChannelStatus status = PushNotificationChannelStatus::InProgress;

            PushNotificationCreateChannelStatus
                channelStatus = { status, channelRequestResult, retryCount };

            progress(channelStatus);

            for (auto backOffTime = c_initialBackoff; ; backOffTime += c_backoffIncrement)
            {
                try
                {
                    ChannelDetails channelInfo{};
                    if (PushNotificationHelpers::IsPackagedAppScenario())
                    {
                        auto appUserModelId{ PushNotificationHelpers::GetAppUserModelId() };
                        THROW_IF_FAILED(CreateChannelWithRemoteIdHelper(appUserModelId, remoteId, channelInfo));
                    }
                    else
                    {
                        // AppId is generated by PushNotificationLongRunningTask singleton
                        wil::unique_cotaskmem_string unpackagedAppUserModelId;
                        RegisterUnpackagedApplicationHelper(remoteId, unpackagedAppUserModelId);
                        THROW_IF_FAILED(CreateChannelWithRemoteIdHelper(unpackagedAppUserModelId, remoteId, channelInfo));

                        auto notificationPlatform{ PushNotificationHelpers::GetNotificationPlatform() };

                        wil::unique_cotaskmem_string processName;
                        THROW_IF_FAILED(GetCurrentProcessPath(processName));
                        THROW_IF_FAILED(notificationPlatform->RegisterLongRunningActivatorWithClsid(processName.get(), GetComServerClsid()));

                        std::wstring toastAppId{ RetrieveNotificationAppId() };
                        THROW_IF_FAILED(notificationPlatform->AddToastRegistrationMapping(processName.get(), toastAppId.c_str()));
                    }

                    // Need to register sink on new channel creation if handlers are registered
                    if (m_foregroundHandlers)
                    {
                        RegisterSinkHelper();
                    }

                    PushNotificationTelemetry::ChannelRequestedByApi(S_OK, remoteId);
                    co_return winrt::make<PushNotificationCreateChannelResult>(
                        winrt::make<PushNotificationChannel>(channelInfo),
                        S_OK,
                        PushNotificationChannelStatus::CompletedSuccess);
                }
                catch (...)
                {
                    auto channelRequestException{ hresult_error(to_hresult(), take_ownership_from_abi) };

                    if ((backOffTime <= c_maxBackoff) && IsChannelRequestRetryable(channelRequestException.code()))
                    {
                        channelStatus.extendedError = channelRequestException.code();
                        channelStatus.status = PushNotificationChannelStatus::InProgressRetry;
                        channelStatus.retryCount = ++retryCount;

                        progress(channelStatus);
                    }
                    else
                    {
                        PushNotificationTelemetry::ChannelRequestedByApi(channelRequestException.code(), remoteId);

                        co_return winrt::make<PushNotificationCreateChannelResult>(
                            nullptr,
                            channelRequestException.code(),
                            PushNotificationChannelStatus::CompletedFailure);
                    }
                }

                co_await winrt::resume_after(backOffTime);
            }
        }
        catch (...)
        {
            PushNotificationTelemetry::ChannelRequestedByApi(wil::ResultFromCaughtException(), remoteId);
            throw;
        }
    }

    void PushNotificationManager::RegisterActivator(PushNotificationActivationInfo const& details)
    {
        THROW_HR_IF(E_NOTIMPL, !::Microsoft::Windows::PushNotifications::Feature_PushNotifications::IsEnabled());

        try
        {
            THROW_HR_IF_NULL(E_INVALIDARG, details);

            auto registrationActivators{ details.Activators() };

            auto isBackgroundTaskFlagSet{ WI_IsAnyFlagSet(registrationActivators, PushNotificationRegistrationActivators::PushTrigger | PushNotificationRegistrationActivators::ComActivator) };
            THROW_HR_IF(E_INVALIDARG, isBackgroundTaskFlagSet && !IsActivatorSupported(registrationActivators));

            auto isProtocolActivatorSet{ WI_IsFlagSet(registrationActivators, PushNotificationRegistrationActivators::ProtocolActivator) };
            THROW_HR_IF(E_INVALIDARG, isProtocolActivatorSet && !IsActivatorSupported(registrationActivators));

            if (isProtocolActivatorSet)
            {
                {
                    auto lock = m_lock.lock_shared();
                    THROW_HR_IF(E_INVALIDARG, m_protocolRegistration);
                }

                // AppId generated by PushNotificationsLongRunningTask singleton
                wil::unique_cotaskmem_string unpackagedAppUserModelId;
                RegisterUnpackagedApplicationHelper(GUID_NULL, unpackagedAppUserModelId); // create default registration for app

                {
                    auto lock = m_lock.lock_exclusive();
                    m_protocolRegistration = true;
                }
            }

            BackgroundTaskBuilder builder{ nullptr };

            if (WI_IsFlagSet(registrationActivators, PushNotificationRegistrationActivators::PushTrigger) && PushNotificationHelpers::IsPackagedAppScenario())
            {
                GUID taskClsid = details.TaskClsid();
                THROW_HR_IF(E_INVALIDARG, taskClsid == GUID_NULL);

                {
                    auto lock = m_lock.lock_shared();
                    THROW_HR_IF(E_INVALIDARG, m_pushTriggerRegistration);
                }

                winrt::hstring taskClsidStr{ winrt::to_hstring(taskClsid) };
                winrt::hstring backgroundTaskFullName{ backgroundTaskName + taskClsidStr };

                auto tasks{ BackgroundTaskRegistration::AllTasks() };
                bool isTaskRegistered{ std::any_of(std::begin(tasks), std::end(tasks),
                    [&](auto&& task)
                    {
                        auto name{ task.Value().Name() };

                        if (std::wstring_view(name).substr(0, backgroundTaskName.size()) != backgroundTaskName)
                        {
                            return false;
                        }

                        if (name == backgroundTaskFullName)
                        {
                            m_pushTriggerRegistration = task.Value();
                            return true;
                        }

                        throw winrt::hresult_invalid_argument(L"RegisterActivator has different clsid registered.");
                    }) };

                if (!isTaskRegistered)
                {
                    builder = BackgroundTaskBuilder();
                    builder.Name(backgroundTaskFullName);

                    PushNotificationTrigger trigger{};
                    builder.SetTrigger(trigger);

                    THROW_HR_IF(E_NOTIMPL, !AppModel::Identity::IsPackagedProcess());

                    // In case the interface is not supported, let it throw.
                    auto builder5{ builder.as<winrt::IBackgroundTaskBuilder5>() };
                    builder5.SetTaskEntryPointClsid(taskClsid);
                    winrt::com_array<winrt::IBackgroundCondition> conditions = details.GetConditions();
                    for (auto condition : conditions)
                    {
                        builder.AddCondition(condition);
                    }
                }
            }

            BackgroundTaskRegistration registeredTaskFromBuilder{ nullptr };

            auto scopeExitToCleanRegistrations{ wil::scope_exit(
                [&]()
                {
                    m_comActivatorRegistration.reset();

                    // Clean the task registration only if it was created during this call
                    if (registeredTaskFromBuilder)
                    {
                        registeredTaskFromBuilder.Unregister(true);
                    }
                }
            ) };

            if (WI_IsFlagSet(registrationActivators, PushNotificationRegistrationActivators::ComActivator))
            {
                GUID taskClsid = details.TaskClsid();
                THROW_HR_IF(E_INVALIDARG, taskClsid == GUID_NULL);

                {
                    auto lock{ m_lock.lock_shared() };
                    THROW_HR_IF_MSG(E_INVALIDARG, m_comActivatorRegistration, "ComActivator already registered.");
                }

                GetWaitHandleForArgs().create();
                GetComServerClsid() = taskClsid;
                
                THROW_IF_FAILED(::CoRegisterClassObject(
                    taskClsid,
                    winrt::make<PushNotificationBackgroundTaskFactory>().get(),
                    CLSCTX_LOCAL_SERVER,
                    REGCLS_MULTIPLEUSE,
                    &m_comActivatorRegistration));
            }

            if (builder)
            {
                registeredTaskFromBuilder = builder.Register();
            }

            scopeExitToCleanRegistrations.release();
            auto lock{ m_lock.lock_exclusive() };
            m_pushTriggerRegistration = registeredTaskFromBuilder;
            PushNotificationTelemetry::ActivatorRegisteredByApi(S_OK, details.Activators());
        }

        catch(...)
        {
            PushNotificationTelemetry::ActivatorRegisteredByApi(wil::ResultFromCaughtException(),
                details == nullptr ? PushNotificationRegistrationActivators::Undefined : details.Activators());
            throw;
        }
    }

    void PushNotificationManager::UnregisterActivator(PushNotificationRegistrationActivators const& activators)
    {
        THROW_HR_IF(E_NOTIMPL, !::Microsoft::Windows::PushNotifications::Feature_PushNotifications::IsEnabled());

        try
        {
            auto lock{ m_lock.lock_exclusive() };
            if (WI_IsFlagSet(activators, PushNotificationRegistrationActivators::PushTrigger))
            {
                THROW_HR_IF_NULL_MSG(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), m_pushTriggerRegistration, "PushTrigger not registered.");
                m_pushTriggerRegistration.Unregister(true);
                m_pushTriggerRegistration = nullptr;
            }

            // Check for COM flag, a valid cookie
            if (WI_IsFlagSet(activators, PushNotificationRegistrationActivators::ComActivator))
            {
                THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), !m_comActivatorRegistration, "ComActivator not registered.");
                m_comActivatorRegistration.reset();
            }

            if (WI_IsFlagSet(activators, PushNotificationRegistrationActivators::ProtocolActivator))
            {
                THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), !m_protocolRegistration);
                auto coInitialize = wil::CoInitializeEx();

                auto notificationPlatform{ PushNotificationHelpers::GetNotificationPlatform() };

                wil::unique_cotaskmem_string processName;
                THROW_IF_FAILED(GetCurrentProcessPath(processName));
                LOG_IF_FAILED(notificationPlatform->UnregisterLongRunningActivator(processName.get()));

                m_protocolRegistration = false;
            }
        }
        catch (...)
        {
            PushNotificationTelemetry::ActivatorUnregisteredByApi(wil::ResultFromCaughtException(), activators);
            throw;
        }

        PushNotificationTelemetry::ActivatorUnregisteredByApi(S_OK, activators);
    }

    void PushNotificationManager::UnregisterAllActivators()
    {
        THROW_HR_IF(E_NOTIMPL, !::Microsoft::Windows::PushNotifications::Feature_PushNotifications::IsEnabled());

        try
        {
            auto lock{ m_lock.lock_exclusive() };
            if (m_pushTriggerRegistration)
            {
                m_pushTriggerRegistration.Unregister(true);
                m_pushTriggerRegistration = nullptr;
            }

            m_comActivatorRegistration.reset();

            if (m_protocolRegistration)
            {
                auto coInitialize = wil::CoInitializeEx();

                auto notificationPlatform{ PushNotificationHelpers::GetNotificationPlatform() };

                wil::unique_cotaskmem_string processName;
                THROW_IF_FAILED(GetCurrentProcessPath(processName));
                LOG_IF_FAILED(notificationPlatform->UnregisterLongRunningActivator(processName.get()));

                m_protocolRegistration = false;
            }
        }
        catch(...)
        {
            PushNotificationTelemetry::ActivatorUnregisteredByApi(wil::ResultFromCaughtException(),
                PushNotificationRegistrationActivators::PushTrigger | PushNotificationRegistrationActivators::ComActivator);
            throw;
        }
        PushNotificationTelemetry::ActivatorUnregisteredByApi(S_OK, PushNotificationRegistrationActivators::PushTrigger | PushNotificationRegistrationActivators::ComActivator);
    }

    winrt::event_token PushNotificationManager::PushReceived(TypedEventHandler<winrt::Microsoft::Windows::PushNotifications::PushNotificationManager, winrt::Microsoft::Windows::PushNotifications::PushNotificationReceivedEventArgs> handler)
    {
        bool registeredEvent{ false };
        {
            auto lock{ m_lock.lock_shared() };
            THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), !m_comActivatorRegistration, "Must call Register() before registering handlers.");
            registeredEvent = bool(m_foregroundHandlers);
        }

        if (!registeredEvent)
        {
            // Need to register sink since foreground handler is being registered
            RegisterSinkHelper();
        }

        auto lock{ m_lock.lock_exclusive() };
        return m_foregroundHandlers.add(handler);
    }


    void PushNotificationManager::PushReceived(winrt::event_token const& token) noexcept
    {
        bool registeredEvent{ false };
        {
            auto lock{ m_lock.lock_exclusive() };
            m_foregroundHandlers.remove(token);
            registeredEvent = bool(m_foregroundHandlers);
        }

        if (!registeredEvent)
        {
            // Packaged apps with BI available will remove their handlers from the platform.
            // Unpackaged apps / Packaged apps treated as unpackaged will unregister from Long Running process.
            if (PushNotificationHelpers::IsPackagedAppScenario())
            {
                auto appUserModelId{ PushNotificationHelpers::GetAppUserModelId() };

                THROW_IF_FAILED(PushNotifications_UnregisterNotificationSinkForFullTrustApplication(appUserModelId.get()));
            }
            else
            {
                auto notificationsLongRunningPlatform{ PushNotificationHelpers::GetNotificationPlatform() };

                wil::unique_cotaskmem_string processName;
                THROW_IF_FAILED(GetCurrentProcessPath(processName));

                THROW_IF_FAILED(notificationsLongRunningPlatform->UnregisterForegroundActivator(processName.get()));
            }
        }
    }

    HRESULT __stdcall PushNotificationManager::InvokeAll(_In_ ULONG length, _In_ byte* payload, _Out_ BOOL* foregroundHandled) noexcept try
    {
        auto args = winrt::make<winrt::Microsoft::Windows::PushNotifications::implementation::PushNotificationReceivedEventArgs>(payload, length);
        m_foregroundHandlers(*this, args);
        *foregroundHandled = args.Handled();
        return S_OK;
    }
    CATCH_RETURN()

    HRESULT __stdcall PushNotificationManager::OnRawNotificationReceived(unsigned int payloadLength, _In_ byte* payload, _In_ HSTRING /*correlationVector */) noexcept try
    {
        BOOL foregroundHandled = true;
        THROW_IF_FAILED(InvokeAll(payloadLength, payload, &foregroundHandled));

        if (!foregroundHandled)
        {
            if (!AppModel::Identity::IsPackagedProcess())
            {
                wil::unique_cotaskmem_string processName;
                THROW_IF_FAILED(GetCurrentProcessPath(processName));
                THROW_IF_FAILED(PushNotificationHelpers::ProtocolLaunchHelper(processName.get(), payloadLength, payload));
            }
        }

        return S_OK;
    }
    CATCH_RETURN();

    bool PushNotificationManager::IsActivatorSupported(PushNotificationRegistrationActivators const& activators)
    {
        THROW_HR_IF(E_NOTIMPL, !::Microsoft::Windows::PushNotifications::Feature_PushNotifications::IsEnabled());

        THROW_HR_IF(E_INVALIDARG, activators == PushNotificationRegistrationActivators::Undefined);

        auto isBackgroundTaskFlagSet{ WI_IsAnyFlagSet(activators, PushNotificationRegistrationActivators::PushTrigger | PushNotificationRegistrationActivators::ComActivator) };
        auto isProtocolActivatorSet{ WI_IsFlagSet(activators, PushNotificationRegistrationActivators::ProtocolActivator) };

        THROW_HR_IF(E_INVALIDARG, isBackgroundTaskFlagSet && isProtocolActivatorSet); // Invalid flag combination
        if (AppModel::Identity::IsPackagedProcess() && isBackgroundTaskFlagSet)
        {
            return true;
        }
        else if(!AppModel::Identity::IsPackagedProcess() && isProtocolActivatorSet)
        {
            return true;
        }
        return false;
    }
}
