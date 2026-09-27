// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include <App/Application.h>

namespace Api
{

// Paths travel as UTF-8 strings on the wire; std::filesystem wants char8_t for that.
inline std::filesystem::path pathFromUtf8(const std::string& text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string utf8FromPath(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

/// A new, empty directory under FreeCAD's temp path, for files that must keep their name.
inline std::string makeTempDir(const std::string& tag)
{
    static std::atomic<int> counter {0};
    const std::string dir = App::Application::getTempPath() + "fcapi-" + tag + "-"
        + std::to_string(++counter) + "-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(pathFromUtf8(dir));
    return dir;
}

}  // namespace Api
