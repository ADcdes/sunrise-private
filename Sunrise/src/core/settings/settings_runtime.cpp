#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../resources/resource.h"
#include "../filesystem/path.h"
#include "../logging/log.h"
#include "settings.h"

namespace sunrise::core::settings {
namespace {

/** The JSON settings file is the only file stored directly in the owned folder. */
constexpr std::wstring_view kSettingsFileSuffix = L"\\settings.json";
/** Largest settings file accepted into fixed stack storage. */
constexpr std::size_t kConfigCapacity = 64 * 1024;

Settings g_settings = defaults();

/**
 * Names the step that ended the load. Settings are read before the log sinks exist, so this line
 * is the only way to report a boot failure here.
 * @param reason Short key naming the step.
 * @return Always false, so callers can return it directly.
 */
[[nodiscard]] bool fail(std::string_view reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings result=fail reason=%.*s",
                                      static_cast<int>(reason.size()),
                                      reason.data());
    if (written > 0) {
        log::early({line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

/**
 * Reports a settings file written against a different layout version.
 *
 * Nothing needs repair: the file is parsed on top of the built-in defaults, so an added key takes
 * its default and a removed key is skipped. This line is the only sign either happened.
 *
 * @param fileVersion Version read from the file, or zero when the key was missing.
 */
void report_version(std::uint32_t fileVersion) noexcept {
    if (fileVersion == kSettingsVersion) {
        return;
    }
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=settings stage=version result=mismatch file=%u build=%u",
                                      static_cast<unsigned>(fileVersion),
                                      static_cast<unsigned>(kSettingsVersion));
    if (written > 0) {
        log::early({line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Copies the bundled default settings. An existing file is never overwritten.
 * @param module Loaded DLL holding the default JSON resource.
 * @param configPath Null-terminated destination path.
 * @return True when every bundled byte is written and the file closes cleanly.
 */
[[nodiscard]] bool write_default(void* module, const path::Buffer& configPath) noexcept {
    const HMODULE loadedModule = static_cast<HMODULE>(module);
    const HRSRC resource =
        FindResourceW(loadedModule, MAKEINTRESOURCEW(IDR_DEFAULT_SETTINGS), RT_RCDATA);
    if (resource == nullptr) {
        return false;
    }
    const DWORD size = SizeofResource(loadedModule, resource);
    const HGLOBAL loaded = LoadResource(loadedModule, resource);
    const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (size == 0 || bytes == nullptr) {
        return false;
    }
    const HANDLE file = CreateFileW(configPath.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool complete = WriteFile(file, bytes, size, &written, nullptr) != FALSE && written == size;
    complete = CloseHandle(file) != FALSE && complete;
    if (!complete) {
        // A half-written default must not become the next boot's settings.
        (void)DeleteFileW(configPath.chars.data());
    }
    return complete;
}

} // namespace

/** Loads the settings file from the owned folder, or creates the default one. */
bool initialize(void* module) noexcept {
    path::Buffer configPath;
    if (!path::artifact_directory(module, configPath)
        || !path::append(configPath, kSettingsFileSuffix)) {
        return fail("path");
    }

    const HANDLE file = CreateFileW(configPath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    HANDLE readableFile = file;
    if (readableFile == INVALID_HANDLE_VALUE) {
        // A missing file is created once; other open failures remain fatal.
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return fail("open");
        }
        if (!write_default(module, configPath)) {
            return fail("write_default");
        }
        readableFile = CreateFileW(configPath.chars.data(),
                                   GENERIC_READ,
                                   FILE_SHARE_READ,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
        if (readableFile == INVALID_HANDLE_VALUE) {
            return fail("reopen");
        }
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(readableFile, &size) || size.QuadPart <= 0) {
        CloseHandle(readableFile);
        return fail("empty");
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > kConfigCapacity) {
        // Silence here reads exactly like a crash, and the cap is the usual cause.
        CloseHandle(readableFile);
        return fail("too_large");
    }

    std::array<char, kConfigCapacity> buffer{};
    DWORD read = 0;
    const bool readOk =
        ReadFile(readableFile, buffer.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr)
            != FALSE
        && read == size.QuadPart;
    const bool closed = CloseHandle(readableFile) != FALSE;
    if (!readOk || !closed) {
        return fail("read");
    }
    Settings parsed;
    if (!parse(std::string_view(buffer.data(), read), parsed)) {
        return fail("parse");
    }
    report_version(parsed.version);
    g_settings = parsed;
    return true;
}

/** Resets active settings to the fixed defaults. */
void shutdown() noexcept {
    g_settings = defaults();
}

/** @return Active read-only Core settings. */
const Settings& get() noexcept {
    return g_settings;
}

} // namespace sunrise::core::settings
