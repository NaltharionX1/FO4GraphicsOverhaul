#include "PCH.h"

#include "Platform/HdrDisplay.h"

#include <dxgi1_6.h>

#include <mutex>

namespace
{
    template <class T>
    class ComHandle
    {
    public:
        ComHandle() noexcept = default;
        ~ComHandle() noexcept { Reset(); }
        ComHandle(const ComHandle&) = delete;
        ComHandle& operator=(const ComHandle&) = delete;

        T** Put() noexcept { Reset(); return &m_ptr; }
        void** PutVoid() noexcept { Reset(); return reinterpret_cast<void**>(&m_ptr); }
        [[nodiscard]] T* operator->() const noexcept { return m_ptr; }
        explicit operator bool() const noexcept { return m_ptr != nullptr; }

        void Reset() noexcept
        {
            if (m_ptr) {
                m_ptr->Release();
                m_ptr = nullptr;
            }
        }

    private:
        T* m_ptr{ nullptr };
    };

    std::mutex g_mutex;
    Platform::HdrDisplay::Info g_info{};

    [[nodiscard]] bool SameReading(const Platform::HdrDisplay::Info& a_lhs,
        const Platform::HdrDisplay::Info& a_rhs) noexcept
    {
        return a_lhs.displayInHdrMode == a_rhs.displayInHdrMode &&
               a_lhs.scrgbPresentable == a_rhs.scrgbPresentable &&
               a_lhs.bitsPerColor == a_rhs.bitsPerColor &&
               a_lhs.colorSpace == a_rhs.colorSpace &&
               a_lhs.maxLuminanceNits == a_rhs.maxLuminanceNits &&
               a_lhs.minLuminanceNits == a_rhs.minLuminanceNits &&
               a_lhs.maxFullFrameNits == a_rhs.maxFullFrameNits;
    }
}

namespace Platform
{
    void HdrDisplay::Observe(IDXGISwapChain* a_swapChain) noexcept
    {
        if (!a_swapChain) {
            return;
        }
        Info reading{};
        try {
            ComHandle<IDXGIOutput> output;
            if (FAILED(a_swapChain->GetContainingOutput(output.Put())) || !output) {
                return;
            }
            ComHandle<IDXGIOutput6> output6;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), output6.PutVoid())) &&
                output6) {
                DXGI_OUTPUT_DESC1 desc{};
                if (SUCCEEDED(output6->GetDesc1(&desc))) {
                    reading.observed = true;
                    reading.displayInHdrMode =
                        desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    reading.maxLuminanceNits = desc.MaxLuminance;
                    reading.minLuminanceNits = desc.MinLuminance;
                    reading.maxFullFrameNits = desc.MaxFullFrameLuminance;
                    reading.bitsPerColor = desc.BitsPerColor;
                    reading.colorSpace = static_cast<std::uint32_t>(desc.ColorSpace);
                }
            }

            ComHandle<IDXGISwapChain3> swapChain3;
            if (SUCCEEDED(a_swapChain->QueryInterface(__uuidof(IDXGISwapChain3),
                    swapChain3.PutVoid())) &&
                swapChain3) {
                UINT support = 0;
                if (SUCCEEDED(swapChain3->CheckColorSpaceSupport(
                        DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support))) {
                    reading.scrgbPresentable =
                        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0U;
                }
            }
        } catch (...) {
            return;
        }
        if (!reading.observed) {
            return;
        }

        bool changed = false;
        {
            const std::scoped_lock lock{ g_mutex };
            changed = !g_info.observed || !SameReading(g_info, reading);
            g_info = reading;
        }
        if (changed) {
            logger::info(
                "[HDR] display: HDR mode {}, {} bpc, peak {:.0f} nits (full-frame {:.0f}, black {:.4f}), "
                "DXGI color space {}; the presenting swap chain scRGB-presentable: {}",
                reading.displayInHdrMode ? "ON" : "off", reading.bitsPerColor,
                static_cast<double>(reading.maxLuminanceNits),
                static_cast<double>(reading.maxFullFrameNits),
                static_cast<double>(reading.minLuminanceNits), reading.colorSpace,
                reading.scrgbPresentable ? "yes" : "no");
        }
    }

    HdrDisplay::Info HdrDisplay::Current() noexcept
    {
        const std::scoped_lock lock{ g_mutex };
        return g_info;
    }
}
