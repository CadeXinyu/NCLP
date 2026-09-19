/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of the NCLP Open Ephys plugin.
 * Portions Copyright (C) 2013 Open Ephys.
 * NCLP modifications Copyright (c) 2026 CadeXinyu.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */

#include "NCLPDataThread.h"

#include <PluginInfo.h>

#ifdef _WIN32
#include <Windows.h>
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

using namespace Plugin;

#define NUM_PLUGINS 1

extern "C" EXPORT void getLibInfo(Plugin::LibraryInfo* info)
{
    info->apiVersion = PLUGIN_API_VER;
    info->name = "NCLP Intan Source Library";
    info->libVersion = "1.0.0";
    info->numPlugins = NUM_PLUGINS;
}

extern "C" EXPORT int getPluginInfo(int index, Plugin::PluginInfo* info)
{
    switch (index)
    {
        case 0:
            info->type = Plugin::DATA_THREAD;
            info->dataThread.name = "NCLP Intan Source";
            info->dataThread.creator = &Plugin::createDataThread<nclp::DataThreadPlugin>;
            break;
        default:
            return -1;
    }

    return 0;
}

#ifdef _WIN32
BOOL WINAPI DllMain(IN HINSTANCE hDllHandle, IN DWORD nReason, IN LPVOID Reserved)
{
    return TRUE;
}
#endif
