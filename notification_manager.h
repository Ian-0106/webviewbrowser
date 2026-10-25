#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <string>

bool InitializeNativeNotifications(HWND targetWindow, UINT activationMessage);
void ShutdownNativeNotifications();
bool ShowNativeDownloadNotification(const std::wstring &id,
                                    const std::wstring &filename,
                                    std::int64_t totalBytes,
                                    bool completed,
                                    const std::wstring &failureReason);
