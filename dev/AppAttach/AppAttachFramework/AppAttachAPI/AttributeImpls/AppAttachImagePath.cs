﻿// Copyright (c) Microsoft Corporation and Contributors.
// Licensed under the MIT License.

using AppAttachAPI.Constants;
using AppAttachAPI.Data;

namespace AppAttachAPI.AttributeImpls
{
    public class AppAttachImagePath : IAttribute
    {
        private string _appAttachImagePath;

        public string getAttributeName()
        {
            return AttrConsts.APPATTACH_IMAGE_PATH;
        }

        public bool getAttributeValidationStatus()
        {
            return true;
        }

        public string getAttributeValue()
        {
            return this._appAttachImagePath;
        }

        public void setAttributeValue(string value)
        {
            this._appAttachImagePath = value;
        }
    }
}