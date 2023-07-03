﻿// Copyright (c) Microsoft Corporation and Contributors.
// Licensed under the MIT License.

using AppAttachAPI.Response.Interfaces;
using System.Collections.Generic;

namespace TroubleshootAppAttachPublish.Interfaces
{
    public interface ITroubleshootAppAttachRegister
    {
        List<IAppAttachOutput> register(string packagePath);

    }
}