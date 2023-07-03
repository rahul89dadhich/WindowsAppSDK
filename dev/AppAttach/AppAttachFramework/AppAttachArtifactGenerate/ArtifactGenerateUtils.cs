﻿// Copyright (c) Microsoft Corporation and Contributors.
// Licensed under the MIT License.

using System;
using System.IO;

namespace AppAttachArtifactGenerate
{
    public static class ArtifactGenerateUtils
    {
        public static double getArtifactSize(string packagePath)
        {
            // this max vhdx size is based on practical observations i.e, maximum size of vhdx after unpacking msix is observed to be this value
            double msixSize = (double)new FileInfo(packagePath).Length / (1024 * 1024);
            double vhdxSize = Math.Pow(2, Math.Ceiling(Math.Log(3 * msixSize) / Math.Log(2)));
            vhdxSize = vhdxSize < 5 ? 5 : vhdxSize;
            return vhdxSize;
        }

    }
}