// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#pragma once
#include "Microsoft.Windows.AppNotifications.Builder.AppNotificationHeader.g.h"

namespace winrt::Microsoft::Windows::AppNotifications::Builder::implementation
{
    struct AppNotificationHeader : AppNotificationHeaderT<AppNotificationHeader, winrt::Windows::Foundation::IStringable>
    {
        AppNotificationHeader() = default;
        AppNotificationHeader(winrt::hstring const& id, winrt::hstring const& title);

        winrt::Microsoft::Windows::AppNotifications::Builder::AppNotificationHeader AddArgument(winrt::hstring const& key, winrt::hstring const& value);

        // IStringable
        winrt::hstring ToString();

    private:
        std::wstring GetArguments();

        winrt::hstring m_id{};
        winrt::hstring m_title{};
        winrt::Windows::Foundation::Collections::IMap<winrt::hstring, winrt::hstring> m_arguments{ winrt::single_threaded_map<winrt::hstring, winrt::hstring>() };

    };
}

namespace winrt::Microsoft::Windows::AppNotifications::Builder::factory_implementation
{
    struct AppNotificationHeader : AppNotificationHeaderT<AppNotificationHeader, implementation::AppNotificationHeader>
    {
    };
}