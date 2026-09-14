#pragma once

#include "Platform/CommandCatalogue.h"

#include "UI/MenuStyle.h"

#include <atomic>
#include <cstdint>
#include <mutex>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace UI
{

    class Menu
    {
    public:
        [[nodiscard]] static Menu& GetSingleton() noexcept
        {
            static Menu instance;
            return instance;
        }

        bool Init(HWND a_hwnd, ID3D11Device* a_device, ID3D11DeviceContext* a_context);

        void HandleDeviceEdge(HWND a_hwnd, ID3D11Device* a_device, ID3D11DeviceContext* a_context);

        void Render();

        void Toggle() { SetOpen(!IsOpen()); }
        void SetOpen(bool a_open);
        [[nodiscard]] bool IsOpen() const noexcept { return isShow_.load(std::memory_order_acquire); }

        void ServicePendingForcedClose();

        [[nodiscard]] static std::recursive_mutex& InputMutex() noexcept
        {
            static std::recursive_mutex instance;
            return instance;
        }

        enum class RebindTarget : std::uint32_t
        {
            kNone = 0,
            kMenuHotkey = 1,
            kOsdHotkey = 2,
            kFingerprintToggle = 3,
            kFingerprintMark = 4,
        };

        void ArmRebind() noexcept
        {
            rebindTarget_.store(RebindTarget::kMenuHotkey, std::memory_order_release);
        }
        void ArmOsdRebind() noexcept
        {
            rebindTarget_.store(RebindTarget::kOsdHotkey, std::memory_order_release);
        }
        [[nodiscard]] bool IsRebindArmed() const noexcept
        {
            return rebindTarget_.load(std::memory_order_acquire) == RebindTarget::kMenuHotkey;
        }
        [[nodiscard]] bool IsOsdRebindArmed() const noexcept
        {
            return rebindTarget_.load(std::memory_order_acquire) == RebindTarget::kOsdHotkey;
        }
        void ArmFingerprintToggleRebind() noexcept
        {
            rebindTarget_.store(RebindTarget::kFingerprintToggle, std::memory_order_release);
        }
        void ArmFingerprintMarkRebind() noexcept
        {
            rebindTarget_.store(RebindTarget::kFingerprintMark, std::memory_order_release);
        }
        [[nodiscard]] bool IsFingerprintToggleRebindArmed() const noexcept
        {
            return rebindTarget_.load(std::memory_order_acquire) == RebindTarget::kFingerprintToggle;
        }
        [[nodiscard]] bool IsFingerprintMarkRebindArmed() const noexcept
        {
            return rebindTarget_.load(std::memory_order_acquire) == RebindTarget::kFingerprintMark;
        }
        bool OfferRebindKey(std::uint32_t a_vk);

        [[nodiscard]] bool IsInitialized() const noexcept
        {
            return initialized_.load(std::memory_order_acquire);
        }

        Menu(const Menu&) = delete;
        Menu& operator=(const Menu&) = delete;

    private:
        Menu() = default;

        void DrawShell();
        void DrawTabGeneral();
        void DrawTabDisplay();
        void DrawTabVisualEffects();
        void DrawAmbientOcclusionControls();
        void DrawCatalogueGroup(const char* a_group);
        static void DrawSectionHeading(const char* a_section);
        static void DrawSectionExtras(const char* a_group, const char* a_section);
        void DrawCatalogueRow(const Platform::CatalogueRow& a_row, std::size_t a_globalIndex);
        void DrawTabOsdUi();
        void DrawTabKeybindings();
        void DrawTabAdvanced();
        void DrawTabDebug();
        void DrawFooter();

        std::atomic<bool> initialized_{ false };
        ID3D11Device* initDevice_{ nullptr };
        std::atomic<bool> isShow_{ false };
        std::atomic<bool> pendingForcedClose_{ false };
        std::atomic<RebindTarget> rebindTarget_{ RebindTarget::kNone };
    };
}
