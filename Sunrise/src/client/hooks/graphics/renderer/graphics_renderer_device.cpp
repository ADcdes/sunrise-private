#include <Windows.h>

#include <dxgi.h>

#include "state.h"

namespace sunrise::client::hooks::graphics::renderer {
namespace {

/** @param result SDK presentation result. @return True for a lost D3D11 device. */
[[nodiscard]] bool is_device_loss(HRESULT result) noexcept {
    return result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DEVICE_REMOVED
           || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

} // namespace

/** Drops our RTV before the original resize call runs. */
void before_resize(IDXGISwapChain* swapChain) noexcept {
    AcquireSRWLockExclusive(&g_rendererLock);
    if (g_resources.swapChain == swapChain) {
        if (g_resources.activeResizeCalls == 0) {
            // The first of any overlapping calls drops the RTV until they have all returned.
            release_render_target(g_resources);
        }
        ++g_resources.activeResizeCalls;
    }
    ReleaseSRWLockExclusive(&g_rendererLock);
}

/** Rebuilds our render state after the original resize returns. */
void after_resize(IDXGISwapChain* swapChain, HRESULT result) noexcept {
    AcquireSRWLockExclusive(&g_rendererLock);
    if (g_resources.swapChain == swapChain && g_resources.activeResizeCalls != 0) {
        g_resources.resizeDeviceLost = g_resources.resizeDeviceLost || is_device_loss(result);
        --g_resources.activeResizeCalls;
        if (g_resources.activeResizeCalls == 0) {
            const bool targetReady = create_render_target(g_resources);
            const bool deviceLost = g_resources.resizeDeviceLost;
            g_resources.resizeDeviceLost = false;
            if (deviceLost || !targetReady) {
                // No RTV goes out until the last overlapping call has returned.
                (void)shutdown_locked();
            }
        }
    }
    ReleaseSRWLockExclusive(&g_rendererLock);
}

/** Frees our resources when Present reports a lost device. */
void present_result(IDXGISwapChain* swapChain, HRESULT result) noexcept {
    if (!is_device_loss(result)) {
        return;
    }
    AcquireSRWLockExclusive(&g_rendererLock);
    if (g_resources.swapChain == swapChain) {
        (void)shutdown_locked();
    }
    ReleaseSRWLockExclusive(&g_rendererLock);
}

} // namespace sunrise::client::hooks::graphics::renderer
