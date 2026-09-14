#include "PCH.h"

#include "Platform/NeuralRenderer.h"

#include "Platform/D3D12Sidecar.h"
#include "Platform/NgxD3D12.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace
{
    using namespace Platform;

    struct PassState
    {
        Ngx::Parameter* params{ nullptr };
        Ngx::Handle* feature{ nullptr };
        NeuralRenderer::FeatureKey key{};
        NeuralRenderer::FeatureKey latchedKey{};
        bool featureLatched{ false };
        char reason[160]{};
        std::uint32_t consecutiveFailures{ 0 };
        std::uint32_t lastResult{ 0 };
        unsigned long lastFault{ 0 };
        std::uint64_t evaluations{ 0 };
        std::uint32_t creates{ 0 };
    };

    struct State
    {
        bool runtimeReady{ false };
        bool runtimeLatched{ false };
        bool faulted{ false };
        ID3D12Device* device{ nullptr };
        char reason[160]{};
        std::uint32_t lastResult{ 0 };
        unsigned long lastFault{ 0 };
        std::array<PassState, Neural::kMaxPasses> passes{};
        NgxD3D12::SnippetIdentity identity{};
        const Neural::RuntimeProfile* profile{ nullptr };
        std::wstring snippetPath;
        std::wstring shimPath;
        std::wstring dataPath;
    };

    State g_state;

    [[nodiscard]] std::string Narrow(const wchar_t* a_text)
    {
        if (a_text == nullptr || *a_text == L'\0') {
            return {};
        }
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            return {};
        }
        std::string out(static_cast<std::size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    constexpr std::uint32_t kEvaluateStrikes = 3;

    [[nodiscard]] std::filesystem::path SelfModuleDirectory()
    {
        wchar_t buffer[MAX_PATH]{};
        HMODULE module = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_state), &module);
        ::GetModuleFileNameW(module, buffer, MAX_PATH);
        return std::filesystem::path{ buffer }.parent_path();
    }

    void Latch(const char* a_reason) noexcept
    {
        if (!g_state.runtimeLatched) {
            g_state.runtimeLatched = true;
            std::snprintf(g_state.reason, sizeof(g_state.reason), "%s", a_reason != nullptr ? a_reason : "unknown");
            logger::warn("[Neural] runtime LATCHED: {}. {}", g_state.reason,
                g_state.faulted ? "The runtime raised an exception: a retry is refused for this session - restart the game."
                                : "Use 'Reset & retry' in the menu after fixing the cause.");
        }
    }

    void ApplyParams(Ngx::Parameter* a_params, const Neural::ParamList& a_list) noexcept
    {
        for (std::size_t i = 0; i < a_list.count; ++i) {
            const Neural::KeyValue& kv = a_list.items[i];
            switch (kv.type) {
            case Neural::ValueType::kUInt: a_params->Set(kv.key, kv.u); break;
            case Neural::ValueType::kInt: a_params->Set(kv.key, kv.i); break;
            case Neural::ValueType::kFloat: a_params->Set(kv.key, kv.f); break;
            case Neural::ValueType::kResource: a_params->Set(kv.key, static_cast<ID3D12Resource*>(kv.p)); break;
            }
        }
    }

    void ClearResourceKeys(PassState& a_pass) noexcept
    {
        if (a_pass.params == nullptr) {
            return;
        }
        static constexpr const char* kKeys[]{ "DLSSNR.Color", "DLSSNR.Output", "DLSSNR.Backbuffer",
            "DLSSNR.Depth", "DLSSNR.MVec" };
        for (const char* key : kKeys) {
            a_pass.params->Set(key, static_cast<ID3D12Resource*>(nullptr));
        }
    }
}

namespace Platform::NeuralRenderer
{
    bool EnsureRuntime(ID3D12Device* a_device) noexcept
    {
        if (a_device == nullptr || g_state.runtimeLatched || g_state.faulted) {
            return false;
        }
        if (g_state.runtimeReady) {
            if (g_state.device == a_device) return true;
            for (const auto& pass : g_state.passes) {
                if (pass.feature != nullptr) {
                    Latch("old-device neural features have not retired");
                    return false;
                }
            }
            ReleaseRuntime();
        }
        try {
            if (g_state.snippetPath.empty()) {
                const auto runtimes = SelfModuleDirectory() / L"FO4GraphicsOverhaul" / L"Streamline";
                const auto ours = SelfModuleDirectory() / L"FO4GraphicsOverhaul" / L"NGX";
                g_state.dataPath = (runtimes / L"").wstring();
                g_state.snippetPath = (runtimes / L"nvngx_dlssnr.dll").wstring();
                g_state.shimPath = (ours / L"FO4GraphicsOverhaul-nvngx.dll").wstring();
                logger::info("[Neural] payload folder: {}", Narrow(g_state.dataPath.c_str()));
            }
            if (!NgxD3D12::LoadCore()) {
                Latch("the NGX core (_nvngx.dll) is unavailable - is the NVIDIA driver installed?");
                return false;
            }
            const NgxD3D12::Outcome session =
                NgxD3D12::InitSession(a_device, g_state.dataPath.c_str(), g_state.dataPath.c_str());
            if (!session.Ok()) {
                char text[128]{};
                NgxD3D12::DescribeOutcome(session, text, sizeof(text));
                g_state.faulted = session.Faulted();
                char reason[160]{};
                std::snprintf(reason, sizeof(reason), "the NGX D3D12 session did not initialise (%s)", text);
                Latch(reason);
                return false;
            }
            if (!NgxD3D12::LoadSnippet(g_state.snippetPath.c_str(), g_state.shimPath.c_str(), g_state.identity)) {
                Latch("the DLSS 5 runtime or its caller shim did not load (see the [NGX] lines above)");
                return false;
            }
            const NgxD3D12::Outcome init = NgxD3D12::SnippetInit(a_device, g_state.dataPath.c_str());
            if (!init.Ok()) {
                char text[128]{};
                NgxD3D12::DescribeOutcome(init, text, sizeof(text));
                g_state.faulted = init.Faulted();
                g_state.lastResult = Ngx::Code(init.result);
                g_state.lastFault = init.faultCode;
                char reason[160]{};
                if (Ngx::Code(init.result) == 0xBAD00002U) {
                    std::snprintf(reason, sizeof(reason),
                        "the runtime refused its caller (%s): the shim DLL's name/frame did not satisfy the caller check", text);
                } else {
                    std::snprintf(reason, sizeof(reason), "the DLSS 5 runtime refused to initialise (%s)", text);
                }
                Latch(reason);
                return false;
            }
            g_state.runtimeReady = true;
            g_state.device = a_device;
            a_device->AddRef();
            logger::info("[Neural] runtime READY: sdk version {:#04x}, runtime {} ({}), feature 18 available to create",
                NgxD3D12::NegotiatedSdkVersion(), g_state.identity.version, g_state.identity.signature);
            g_state.profile = Neural::FindRuntimeProfile(g_state.identity.sha256);
            if (g_state.profile != nullptr && g_state.profile->validated) {
                logger::info("[Neural] runtime profile: {} — {}", g_state.profile->label, g_state.profile->tested);
            } else if (g_state.profile != nullptr) {
                logger::warn("[Neural] runtime profile: {} — {}. UNVALIDATED: the 310.8 parameter policy is applied unverified", g_state.profile->label, g_state.profile->tested);
            } else {
                logger::warn("[Neural] runtime profile: none for sha256 {}. UNVALIDATED: the 310.8 parameter policy is applied unverified; test this runtime in isolation before trusting any control", g_state.identity.sha256);
            }
            return true;
        } catch (...) {
            Latch("C++ exception bringing the runtime up");
            return false;
        }
    }

    bool RuntimeReady() noexcept { return g_state.runtimeReady && !g_state.runtimeLatched; }

    bool EnsureFeature(const FeatureKey& a_key, const Neural::Shape& a_shape,
        const Neural::Settings& a_settings, const Neural::Resources& a_resources, std::uint32_t a_stage) noexcept
    {
        if (a_stage >= Neural::kMaxPasses || !RuntimeReady()) {
            return false;
        }
        auto& pass = g_state.passes[a_stage];
        if (pass.feature != nullptr && !pass.featureLatched && pass.key == a_key) {
            return true;
        }
        if (pass.featureLatched && pass.latchedKey == a_key) {
            return false;
        }
        if (pass.params == nullptr) {
            pass.params = NgxD3D12::AllocateParameters();
            if (pass.params == nullptr) {
                pass.featureLatched = true;
                pass.latchedKey = a_key;
                std::snprintf(pass.reason, sizeof(pass.reason), "pass %u parameter allocation failed", a_stage + 1U);
                logger::error("[Neural] {}", pass.reason);
                if (NgxD3D12::CoreFaulted()) {
                    g_state.faulted = true;
                    Latch("NGX faulted allocating a stage's parameters");
                }
                return false;
            }
        }
        DestroyFeature(a_stage);

        ID3D12GraphicsCommandList* list = nullptr;
        if (!D3D12Sidecar::BeginCommands(list)) {
            return false;
        }
        const Neural::ParamList params = Neural::BuildParams(a_shape, a_settings, a_resources, true);
        ApplyParams(pass.params, params);
        Ngx::Handle* handle = nullptr;
        const NgxD3D12::Outcome create = NgxD3D12::SnippetCreate(list, Ngx::kFeatureNeuralRendering, pass.params, &handle);
        if (create.faultCode != 0) {
            (void)D3D12Sidecar::AbandonCommands();
            pass.lastResult = Ngx::Code(create.result);
            pass.lastFault = create.faultCode;
            g_state.faulted = true;
            char text[128]{};
            NgxD3D12::DescribeOutcome(create, text, sizeof(text));
            char reason[160]{};
            std::snprintf(reason, sizeof(reason), "CreateFeature(18) raised %s", text);
            Latch(reason);
            return false;
        }
        std::uint32_t aliasOwner = Neural::kMaxPasses;
        if (create.faultCode == 0 && handle != nullptr) {
            for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
                if (i != a_stage && g_state.passes[i].feature == handle) {
                    aliasOwner = i;
                    break;
                }
            }
        }
        pass.feature = create.faultCode == 0 && aliasOwner == Neural::kMaxPasses ? handle : nullptr;
        bool listAccepted = false;
        const std::uint64_t value = D3D12Sidecar::EndCommands(&listAccepted);
        const bool completed = value != 0 && D3D12Sidecar::WaitForValue(value, 4000);

        pass.lastResult = Ngx::Code(create.result);
        pass.lastFault = create.faultCode;
        if (!completed) {
            D3D12Sidecar::Disable("feature creation did not complete within 4 s");
            Latch("feature creation did not complete on the GPU");
            return false;
        }
        if (aliasOwner != Neural::kMaxPasses) {
            pass.featureLatched = true;
            pass.latchedKey = a_key;
            std::snprintf(pass.reason, sizeof(pass.reason), "the runtime reused pass %u's feature handle", aliasOwner + 1U);
            logger::error("[Neural] pass {} refused: {}", a_stage + 1U, pass.reason);
            Latch("the runtime did not create independent neural features");
            return false;
        }
        if (!Neural::CanUseStageOutput(Ngx::Succeeded(create.result), listAccepted, value, completed) || handle == nullptr) {
            pass.featureLatched = true;
            pass.latchedKey = a_key;
            std::snprintf(pass.reason, sizeof(pass.reason),
                "CreateFeature(18) %s (%#010x) for %ux%u fmt=%d preset=%u",
                Ngx::ResultName(create.result), Ngx::Code(create.result), a_key.outWidth, a_key.outHeight,
                static_cast<int>(a_key.carrierFormat), a_key.preset);
            if (!listAccepted) std::snprintf(pass.reason, sizeof(pass.reason), "the feature creation command list was rejected");
            logger::error("[Neural] pass {}: {}", a_stage + 1U, pass.reason);
            if (handle != nullptr) {
                DestroyFeature(a_stage);
            }
            if (Ngx::Code(create.result) == 0xBAD00002U) {
                logger::error("[Neural]   0xBAD00002 at create = the caller check: the shim frame did not satisfy it");
            }
            return false;
        }
        pass.feature = handle;
        pass.featureLatched = false;
        pass.reason[0] = '\0';
        pass.key = a_key;
        pass.consecutiveFailures = 0;
        ++pass.creates;
        logger::info("[Neural] pass {} feature 18 created (handle {}): {}x{} carrier fmt={} preset={} guides {}x{} (create #{}; style is live)",
            a_stage + 1U, fmt::ptr(handle), a_key.outWidth, a_key.outHeight, static_cast<int>(a_key.carrierFormat), a_key.preset,
            a_shape.renderWidth, a_shape.renderHeight, pass.creates);
        return true;
    }

    bool FeatureReady(std::uint32_t a_stage) noexcept
    {
        return a_stage < Neural::kMaxPasses && g_state.passes[a_stage].feature != nullptr &&
            !g_state.passes[a_stage].featureLatched && RuntimeReady();
    }
    FeatureKey CurrentKey(std::uint32_t a_stage) noexcept
    {
        return a_stage < Neural::kMaxPasses ? g_state.passes[a_stage].key : FeatureKey{};
    }

    bool HasResources(std::uint32_t a_stage) noexcept
    {
        return a_stage < Neural::kMaxPasses &&
            (g_state.passes[a_stage].feature != nullptr || g_state.passes[a_stage].params != nullptr);
    }

    bool Evaluate(ID3D12GraphicsCommandList* a_list, const Neural::Shape& a_shape,
        const Neural::Settings& a_settings, const Neural::Resources& a_resources, bool a_reset, std::uint32_t a_stage) noexcept
    {
        if (a_stage >= Neural::kMaxPasses) {
            return false;
        }
        auto& pass = g_state.passes[a_stage];
        if (a_list == nullptr || !FeatureReady(a_stage) || pass.params == nullptr) {
            return false;
        }
        const Neural::ParamList params = Neural::BuildParams(a_shape, a_settings, a_resources, a_reset);
        ApplyParams(pass.params, params);
        const NgxD3D12::Outcome eval = NgxD3D12::SnippetEvaluate(a_list, pass.feature, pass.params);
        pass.lastResult = Ngx::Code(eval.result);
        pass.lastFault = eval.faultCode;
        if (eval.faultCode != 0) {
            g_state.faulted = true;
            char text[128]{};
            NgxD3D12::DescribeOutcome(eval, text, sizeof(text));
            char reason[160]{};
            std::snprintf(reason, sizeof(reason), "EvaluateFeature(18) raised %s", text);
            Latch(reason);
            return false;
        }
        if (!Ngx::Succeeded(eval.result)) {
            if (++pass.consecutiveFailures <= kEvaluateStrikes) {
                logger::error("[Neural] pass {} evaluate failed {:#010x} {} (strike {}/{})", a_stage + 1U, Ngx::Code(eval.result),
                    Ngx::ResultName(eval.result), pass.consecutiveFailures, kEvaluateStrikes);
            }
            if (pass.consecutiveFailures >= kEvaluateStrikes) {
                pass.featureLatched = true;
                pass.latchedKey = pass.key;
                std::snprintf(pass.reason, sizeof(pass.reason), "pass %u evaluate failed %u times", a_stage + 1U, kEvaluateStrikes);
                logger::warn("[Neural] {}; the preceding image is retained", pass.reason);
            }
            return false;
        }
        pass.consecutiveFailures = 0;
        ++pass.evaluations;
        return true;
    }

    void DestroyFeature(std::uint32_t a_stage) noexcept
    {
        if (a_stage >= Neural::kMaxPasses) {
            return;
        }
        auto& pass = g_state.passes[a_stage];
        if (pass.feature != nullptr) {
            const NgxD3D12::Outcome release = NgxD3D12::SnippetRelease(pass.feature);
            if (release.faultCode != 0) {
                g_state.faulted = true;
                Latch("ReleaseFeature(18) raised an exception");
            }
            pass.feature = nullptr;
            logger::info("[Neural] pass {} feature 18 released after {} evaluation(s)", a_stage + 1U, pass.evaluations);
        }
        ClearResourceKeys(pass);
        pass.key = FeatureKey{};
    }

    void AbandonPass(std::uint32_t a_stage) noexcept
    {
        if (a_stage >= Neural::kMaxPasses) {
            return;
        }
        auto& pass = g_state.passes[a_stage];
        if (pass.feature != nullptr) {
            logger::warn("[Neural] pass {} feature 18 ABANDONED after {} evaluation(s): retirement unproven, the handle is leaked, not released",
                a_stage + 1U, pass.evaluations);
            pass.feature = nullptr;
        }
        ClearResourceKeys(pass);
        pass.key = FeatureKey{};
    }

    void ReleasePass(std::uint32_t a_stage) noexcept
    {
        if (a_stage >= Neural::kMaxPasses) {
            return;
        }
        DestroyFeature(a_stage);
        auto& pass = g_state.passes[a_stage];
        if (pass.params != nullptr) {
            NgxD3D12::DestroyParameters(pass.params);
            pass.params = nullptr;
        }
    }

    void ReleaseRuntime() noexcept
    {
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            ReleasePass(i);
        }
        NgxD3D12::UnloadSnippet();
        g_state.runtimeReady = false;
        if (g_state.device != nullptr) {
            g_state.device->Release();
            g_state.device = nullptr;
        }
    }

    void RearmLatch(std::uint32_t a_stageMask) noexcept
    {
        if (g_state.faulted) {
            logger::warn("[Neural] retry refused: the runtime raised an exception this session; restart the game");
            return;
        }
        for (std::uint32_t i = 0; i < Neural::kMaxPasses; ++i) {
            if ((a_stageMask & (1U << i)) == 0U) {
                continue;
            }
            auto& pass = g_state.passes[i];
            pass.reason[0] = '\0';
            pass.featureLatched = false;
            pass.latchedKey = FeatureKey{};
            pass.consecutiveFailures = 0;
        }
        if (a_stageMask == Neural::kAllPasses && g_state.runtimeLatched) {
            g_state.runtimeLatched = false;
            g_state.reason[0] = '\0';
        }
    }

    State Snapshot(std::uint32_t a_stage) noexcept
    {
        State state{};
        if (a_stage >= Neural::kMaxPasses) {
            std::snprintf(state.reason, sizeof(state.reason), "invalid neural stage");
            return state;
        }
        const auto& pass = g_state.passes[a_stage];
        state.runtimeReady = RuntimeReady();
        state.featureReady = FeatureReady(a_stage);
        state.latched = g_state.runtimeLatched || pass.featureLatched;
        state.faulted = g_state.faulted;
        state.lastResult = g_state.runtimeLatched ? g_state.lastResult : pass.lastResult;
        state.lastFault = g_state.runtimeLatched ? g_state.lastFault : pass.lastFault;
        state.evaluations = pass.evaluations;
        state.creates = pass.creates;
        state.handleAddress = reinterpret_cast<std::uintptr_t>(pass.feature);
        state.sdkVersion = NgxD3D12::NegotiatedSdkVersion();
        std::memcpy(state.reason, g_state.runtimeLatched ? g_state.reason : pass.reason, sizeof(state.reason));
        std::memcpy(state.runtimeVersion, g_state.identity.version, sizeof(state.runtimeVersion));
        std::memcpy(state.runtimeSignature, g_state.identity.signature, sizeof(state.runtimeSignature));
        std::memcpy(state.runtimeSha, g_state.identity.sha256, 16);
        state.profileLabel = g_state.profile != nullptr ? g_state.profile->label : "none (unvalidated)";
        state.profileValidated = g_state.profile != nullptr && g_state.profile->validated;
        state.runtimeSha[16] = '\0';
        return state;
    }
}
