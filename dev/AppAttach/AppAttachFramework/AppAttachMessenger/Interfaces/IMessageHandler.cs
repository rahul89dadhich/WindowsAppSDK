﻿// Copyright (c) Microsoft Corporation and Contributors.
// Licensed under the MIT License.

namespace AppAttachMessenger.Interface
{
    /// <summary>
    /// Represents a contract for handling incoming messages.
    /// </summary>
    public interface IMessageHandler
    {
        /// <summary>
        /// Implementing classes should provide their own implementation for processing the message.
        /// </summary>
        /// <param name="message"></param>
        /// <returns>Returns the response generated by the message handler.</returns>
        string HandleMessage(Message message);
    }
}