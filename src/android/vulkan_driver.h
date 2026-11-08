#pragma once

namespace android_driver {
bool Supported();
bool Requested();
bool Select(bool turnip);
bool Initialize();
bool IsTurnip();
}  // namespace android_driver
