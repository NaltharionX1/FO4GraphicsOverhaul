#include "PCH.h"

#include "Platform/D3D12Sidecar.h"
#include "Platform/Fallout4Renderer.h"
#include "Platform/NeuralPass.h"
#include "Platform/NeuralRenderer.h"
#include "Platform/SidecarCompute.h"
#include "Platform/SidecarFrame.h"
#include "Platform/Streamline.h"
#include "NeuralRendererStub.h"
#include "RE/Bethesda/BSGraphics.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool a_condition, const char* a_message)
    {
        ++g_checks;
        if (!a_condition) {
            ++g_failures;
            std::printf("  - FAIL: %s\n", a_message);
        }
    }

    constexpr std::uint32_t kWidth = 128;
    constexpr std::uint32_t kHeight = 64;

    RE::BSGraphics::RendererData g_rendererData{};
    std::uint64_t g_epoch = 1;

    ComPtr<ID3D11Device> g_device;
    ComPtr<ID3D11DeviceContext> g_context;
    ComPtr<ID3D11Texture2D> g_rt0;
    ComPtr<ID3D11Texture2D> g_depth;
    ComPtr<ID3D11Texture2D> g_motion;
    ComPtr<ID3D11Texture2D> g_staging;

    [[nodiscard]] std::uint8_t Gradient(std::uint32_t a_x) noexcept { return static_cast<std::uint8_t>(a_x * 2U); }
    bool g_alternating = false;
    [[nodiscard]] std::uint8_t Alternating(std::uint32_t a_x) noexcept { return (a_x & 1U) != 0U ? 255U : 0U; }
    [[nodiscard]] std::uint8_t BaseRed(std::uint32_t a_x) noexcept { return g_alternating ? Alternating(a_x) : Gradient(a_x); }

    void FillGradient()
    {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4U);
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            for (std::uint32_t x = 0; x < kWidth; ++x) {
                std::uint8_t* const p = pixels.data() + (static_cast<std::size_t>(y) * kWidth + x) * 4U;
                p[0] = BaseRed(x);
                p[1] = static_cast<std::uint8_t>(y * 4U);
                p[2] = 128;
                p[3] = 255;
            }
        }
        g_context->UpdateSubresource(g_rt0.Get(), 0, nullptr, pixels.data(), kWidth * 4U, 0);
    }

    [[nodiscard]] bool ReadRow0(std::uint8_t (&a_row)[kWidth * 4])
    {
        g_context->CopyResource(g_staging.Get(), g_rt0.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_context->Map(g_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        std::memcpy(a_row, mapped.pData, sizeof(a_row));
        g_context->Unmap(g_staging.Get(), 0);
        return true;
    }

    void ExpectShift(std::uint32_t a_shift, const char* a_label)
    {
        std::uint8_t row[kWidth * 4]{};
        if (!ReadRow0(row)) {
            Check(false, "RT0 could not be read back");
            return;
        }
        std::uint32_t mismatches = 0;
        std::uint32_t firstX = 0;
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const std::uint32_t source = x >= a_shift ? x - a_shift : 0U;
            if (row[x * 4] != Gradient(source)) {
                if (mismatches == 0) firstX = x;
                ++mismatches;
            }
        }
        char message[200]{};
        std::snprintf(message, sizeof(message), "%s: every pixel of RT0 is the gradient shifted by %u stage(s)", a_label, a_shift);
        Check(mismatches == 0, message);
        if (mismatches != 0) {
            const std::uint32_t source = firstX >= a_shift ? firstX - a_shift : 0U;
            std::printf("    (%u mismatching pixel(s); first at x=%u: read %u, expected %u)\n", mismatches, firstX,
                row[firstX * 4], Gradient(source));
        }
    }

    void RunFrame()
    {
        FillGradient();
        Platform::NeuralPass::ExecuteAfterEffectRange();
    }

    void ExpectOffset(double a_offset, std::uint32_t a_firstX, double a_tolerance, const char* a_label)
    {
        std::uint8_t row[kWidth * 4]{};
        if (!ReadRow0(row)) {
            Check(false, "RT0 could not be read back");
            return;
        }
        std::uint32_t mismatches = 0, firstX = 0, channelMismatches = 0;
        double worst = 0.0;
        for (std::uint32_t x = a_firstX; x < kWidth; ++x) {
            const double expected = static_cast<double>(BaseRed(x)) + a_offset;
            const double clamped = expected < 0.0 ? 0.0 : expected > 255.0 ? 255.0 : expected;
            const double error = std::fabs(static_cast<double>(row[x * 4]) - clamped);
            if (error > worst) worst = error;
            if (error > a_tolerance) {
                if (mismatches == 0) firstX = x;
                ++mismatches;
            }
            if (row[x * 4 + 1] != 0U || row[x * 4 + 2] != 128U || row[x * 4 + 3] != 255U) ++channelMismatches;
        }
        char message[240]{};
        std::snprintf(message, sizeof(message), "%s: from x=%u every pixel of RT0 is the base %+.2f within %.2f (worst %.2f)",
            a_label, a_firstX, a_offset, a_tolerance, worst);
        Check(mismatches == 0, message);
        if (mismatches != 0) {
            std::printf("    (%u mismatching pixel(s); first at x=%u: read %u, expected %.2f)\n", mismatches, firstX, row[firstX * 4],
                static_cast<double>(BaseRed(firstX)) + a_offset);
        }
        std::snprintf(message, sizeof(message), "%s: green, blue and alpha are untouched", a_label);
        Check(channelMismatches == 0, message);
    }

    [[nodiscard]] bool RunUntilDelivered(std::uint32_t a_stages, unsigned a_maxFrames);

    [[nodiscard]] bool SetModelPercent(Settings::MenuSettings& a_settings, std::uint32_t a_percent, std::uint32_t a_stages)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        a_settings.neural.modelPercent = a_percent;
        a_settings.neural.modelBeyondPlay = a_percent > Platform::Neural::kModelPercentPlayMax;
        Platform::NeuralPass::ApplyMenuSettings(a_settings);
        for (int i = 0; i < 8; ++i) RunFrame();
        return RunUntilDelivered(a_stages, 40);
    }

    [[nodiscard]] bool RunUntilDelivered(std::uint32_t a_stages, unsigned a_maxFrames)
    {
        for (unsigned frame = 0; frame < a_maxFrames; ++frame) {
            RunFrame();
            if (Platform::NeuralPass::Snapshot().frameStagesDelivered == a_stages) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool CreateTexture(DXGI_FORMAT a_format, UINT a_bind, D3D11_USAGE a_usage, UINT a_cpu, ComPtr<ID3D11Texture2D>& a_out)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = a_format;
        desc.SampleDesc.Count = 1;
        desc.Usage = a_usage;
        desc.BindFlags = a_bind;
        desc.CPUAccessFlags = a_cpu;
        return SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &a_out)) && a_out != nullptr;
    }

    void ReportCounters(const char* a_label)
    {
        const auto s = Platform::NeuralPass::Snapshot();
        std::printf("  %s: %u stage(s) delivered, %u colour copies, %u cross-API return(s); status: %s\n", a_label,
            s.frameStagesDelivered, s.frameColourCopies, s.frameCrossApiTrips, s.status);
    }

    bool g_debug12 = false;
    bool g_debug11 = false;
    bool g_routeProbeSeen = false;

    [[nodiscard]] bool IsRouteProbe(const char* a_description, std::size_t a_length) noexcept
    {
        const std::string_view text{ a_description, a_length };
        return text.find("OpenSharedResource1") != std::string_view::npos && text.find("E_INVALIDARG") != std::string_view::npos;
    }

    void EnableDebugLayer12()
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) {
            debug->EnableDebugLayer();
            g_debug12 = true;
        }
    }

    template <class Queue, class Message, class Severity>
    [[nodiscard]] unsigned DrainQueue(Queue* a_queue, Severity a_corruption, Severity a_error, const char* a_api, const char* a_label)
    {
        unsigned errors = 0;
        const UINT64 count = a_queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T length = 0;
            if (FAILED(a_queue->GetMessage(i, nullptr, &length)) || length == 0) continue;
            std::vector<std::uint8_t> bytes(length);
            auto* const message = reinterpret_cast<Message*>(bytes.data());
            if (FAILED(a_queue->GetMessage(i, message, &length))) continue;
            if (message->Severity != a_corruption && message->Severity != a_error) continue;
            if (!g_routeProbeSeen && IsRouteProbe(message->pDescription, message->DescriptionByteLength)) {
                g_routeProbeSeen = true;
                std::printf("    %s [%s]: the sidecar's one-time sharing-route probe (route A refused, route B taken), expected once\n", a_api, a_label);
                continue;
            }
            ++errors;
            if (errors <= 6) {
                std::printf("    %s validation %s [%s]: %.*s\n", a_api, message->Severity == a_corruption ? "CORRUPTION" : "ERROR",
                    a_label, static_cast<int>(message->DescriptionByteLength), message->pDescription);
            }
        }
        a_queue->ClearStoredMessages();
        return errors;
    }

    void DrainDebugMessages(const char* a_label)
    {
        unsigned errors = 0;
        if (g_debug12) {
            ComPtr<ID3D12InfoQueue> queue;
            ID3D12Device* const device = Platform::D3D12Sidecar::Device();
            if (device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))) && queue) {
                errors += DrainQueue<ID3D12InfoQueue, D3D12_MESSAGE>(queue.Get(), D3D12_MESSAGE_SEVERITY_CORRUPTION,
                    D3D12_MESSAGE_SEVERITY_ERROR, "D3D12", a_label);
            }
        }
        if (g_debug11) {
            ComPtr<ID3D11InfoQueue> queue;
            if (SUCCEEDED(g_device->QueryInterface(IID_PPV_ARGS(&queue))) && queue) {
                errors += DrainQueue<ID3D11InfoQueue, D3D11_MESSAGE>(queue.Get(), D3D11_MESSAGE_SEVERITY_CORRUPTION,
                    D3D11_MESSAGE_SEVERITY_ERROR, "D3D11", a_label);
            }
        }
        if (g_debug12 || g_debug11) {
            char message[200]{};
            std::snprintf(message, sizeof(message), "%s: the debug layer(s) stored no validation error (%u)", a_label, errors);
            Check(errors == 0, message);
        }
    }

    void ExpectRetired(const char* a_label)
    {
        const auto s = Platform::NeuralPass::Snapshot();
        bool retired = true;
        for (std::uint32_t i = 0; i < Platform::Neural::kMaxPasses; ++i) {
            retired = retired && !Platform::NeuralRendererStub::FeatureAlive(i) && !s.passes[i].ready;
        }
        char message[200]{};
        std::snprintf(message, sizeof(message), "%s: every stage's feature is destroyed and no pass is ready", a_label);
        Check(retired, message);
    }
}

RE::BSGraphics::RendererData* RE::BSGraphics::RendererData::GetSingleton() noexcept { return &g_rendererData; }

namespace Platform
{
    FrameInputs Fallout4Renderer::Snapshot() noexcept
    {
        FrameInputs inputs{};
        inputs.color = g_rt0.Get();
        inputs.depth = g_depth.Get();
        inputs.motionVectors = g_motion.Get();
        inputs.displayWidth = kWidth;
        inputs.displayHeight = kHeight;
        inputs.renderWidth = kWidth;
        inputs.renderHeight = kHeight;
        return inputs;
    }
    CameraConstants Fallout4Renderer::CameraSnapshot() noexcept { return {}; }
    std::uint64_t Fallout4Renderer::HistoryEpoch() noexcept { return g_epoch; }
    const char* Fallout4Renderer::HistoryEpochReason() noexcept { return "fixture"; }
    AaRequest Streamline::AppliedRequest() noexcept { return {}; }
    AaEffectiveEngine Streamline::AppliedEffectiveEngine() noexcept { return AaEffectiveEngine::kNone; }
}

int main()
{
    std::printf("NeuralCascadeGpuTests: %ux%u gradient, three-stage cascade, shift-by-one stub model\n", kWidth, kHeight);

    EnableDebugLayer12();
    const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL level{};
    bool warp = false;
    HRESULT hr = E_FAIL;
    for (const D3D_DRIVER_TYPE driver : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP }) {
        for (const UINT flags : { static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG), 0U }) {
            hr = ::D3D11CreateDevice(nullptr, driver, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &g_device, &level, &g_context);
            if (SUCCEEDED(hr)) {
                g_debug11 = flags != 0U;
                break;
            }
        }
        if (SUCCEEDED(hr)) break;
        warp = true;
    }
    Check(SUCCEEDED(hr) && g_device && g_context, "a D3D11 device exists (hardware, else WARP)");
    if (FAILED(hr) || !g_device || !g_context) {
        std::printf("NeuralCascadeGpuTests: %d failure(s) of %d\n", g_failures, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("  device: %s, feature level %#x; debug layers: D3D12 %s, D3D11 %s\n", warp ? "WARP" : "hardware",
        static_cast<unsigned>(level), g_debug12 ? "on" : "unavailable", g_debug11 ? "on" : "unavailable");
    if (!g_debug12) {
        std::printf("  resource-state validation is NOT proven on this machine (no D3D12 debug layer: install Graphics Tools); pixels only\n");
    }

    const bool textures =
        CreateTexture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, g_rt0) &&
        CreateTexture(DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, g_depth) &&
        CreateTexture(DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, g_motion) &&
        CreateTexture(DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, g_staging);
    Check(textures, "RT0, depth, motion vectors and the staging copy exist");
    if (!textures) {
        std::printf("NeuralCascadeGpuTests: %d failure(s) of %d\n", g_failures, g_checks);
        return EXIT_FAILURE;
    }
    g_rendererData.device = g_device.Get();
    g_rendererData.context = g_context.Get();
    g_rendererData.renderTargets[0].texture = g_rt0.Get();
    g_rendererData.depthStencilTargets[2].texture = g_depth.Get();
    g_rendererData.renderTargets[29].texture = g_motion.Get();

    Settings::MenuSettings settings{};
    settings.neural.enabled = true;
    settings.neural.passCount = 3;
    Platform::NeuralPass::ApplyMenuSettings(settings);
    Check(RunUntilDelivered(3, 40), "the cascade delivers all three stages within forty frames (settle window + rebuild)");
    ReportCounters("three stages");
    {
        const auto s = Platform::NeuralPass::Snapshot();
        Check(s.activePasses == 3, "three passes are active");
        Check(Platform::NeuralRendererStub::Evaluates(0) > 0 && Platform::NeuralRendererStub::Evaluates(2) > 0,
            "the stub model was evaluated for the first and the third stage");
        ExpectShift(3, "three stages");
        Check(s.frameColourCopies == 2, "target: a three-stage frame is two colour copies (the copy-in, the copy-home)");
        Check(s.frameCrossApiTrips == 1, "target: a three-stage frame is one cross-API return");
        Check(Platform::SidecarFrame::Snapshot().hudlessLive, "the delivered cascade published its HUD-less image");
    }
    RunFrame();
    ExpectShift(3, "three stages, the next frame");
    DrainDebugMessages("three stages");

    Platform::NeuralRendererStub::FailStage(2);
    for (int i = 0; i < 3; ++i) RunFrame();
    ReportCounters("stage 3 refused");
    {
        const auto s = Platform::NeuralPass::Snapshot();
        Check(s.frameStagesDelivered == 2, "a refused stage 3 delivers the accepted prefix: two stages");
        ExpectShift(2, "stage 3 refused");
        Check(s.frameColourCopies == 2, "target: the refused frame is still two colour copies");
        Check(s.frameCrossApiTrips == 1, "target: the refused frame is still one cross-API return");
        Check(Platform::SidecarFrame::Snapshot().hudlessLive, "the delivered prefix published its HUD-less image");
        Check(Platform::NeuralRendererStub::Latched(2) && s.passes[2].latched, "three refusals in a row latched stage 3 in the renderer and in the pass");
    }
    DrainDebugMessages("stage 3 refused");

    Platform::NeuralRendererStub::FailStage(Platform::NeuralRendererStub::kNoFailure);
    for (int i = 0; i < 3; ++i) RunFrame();
    Check(Platform::NeuralPass::Snapshot().frameStagesDelivered == 2, "a latched stage stays out without a retry (two stages still)");
    Platform::NeuralPass::RequestRetry();
    Check(RunUntilDelivered(3, 40), "after a retry the cascade delivers all three stages again");
    ExpectShift(3, "after the retry");
    DrainDebugMessages("the retry");

    Platform::NeuralRendererStub::FailStage(1);
    for (int i = 0; i < 3; ++i) RunFrame();
    ReportCounters("stage 2 refused");
    {
        const auto s = Platform::NeuralPass::Snapshot();
        Check(s.frameStagesDelivered == 1, "a refused stage 2 delivers the accepted prefix: one stage");
        ExpectShift(1, "stage 2 refused");
        Check(s.frameColourCopies == 2, "target: the refused frame is still two colour copies");
        Check(s.frameCrossApiTrips == 1, "target: the refused frame is still one cross-API return");
        Check(Platform::SidecarFrame::Snapshot().hudlessLive, "the delivered one-stage prefix published its HUD-less image");
    }
    Platform::NeuralRendererStub::FailStage(Platform::NeuralRendererStub::kNoFailure);
    Platform::NeuralPass::RequestRetry();
    Check(RunUntilDelivered(3, 40), "after the second retry the cascade delivers all three stages again");
    DrainDebugMessages("stage 2 refused");

    Platform::NeuralRendererStub::FailStage(0);
    for (int i = 0; i < 3; ++i) RunFrame();
    ReportCounters("stage 1 refused");
    {
        const auto s = Platform::NeuralPass::Snapshot();
        Check(s.frameStagesDelivered == 0, "a refused stage 1 delivers nothing");
        ExpectShift(0, "stage 1 refused");
        Check(s.frameColourCopies == 1, "a refused stage 1 made its copy-in (RT0 into its private input) and no copy-home");
        Check(s.frameCrossApiTrips == 0, "a refused stage 1 makes no cross-API return");
        Check(!Platform::SidecarFrame::Snapshot().hudlessLive, "a refused stage 1 publishes no HUD-less image (never an uninitialised output)");
    }
    DrainDebugMessages("stage 1 refused");

    Platform::NeuralRendererStub::FailStage(Platform::NeuralRendererStub::kNoFailure);
    Platform::NeuralPass::RequestRetry();
    settings.neural.enabled = false;
    Platform::NeuralPass::ApplyMenuSettings(settings);
    for (int i = 0; i < 6; ++i) RunFrame();
    ExpectRetired("disabled before the mixed cascade");
    Platform::NeuralRendererStub::RefuseNativeCarrier(1);
    settings.neural.enabled = true;
    settings.neural.passCount = 2;
    Platform::NeuralPass::ApplyMenuSettings(settings);
    Check(RunUntilDelivered(2, 60), "the mixed cascade (native, then FP16) delivers both stages within sixty frames");
    ReportCounters("mixed cascade");
    {
        const auto s = Platform::NeuralPass::Snapshot();
        Check(std::strcmp(s.passes[0].carrier, "native") == 0 && std::strcmp(s.passes[1].carrier, "FP16") == 0,
            "stage 1 runs on the native carrier and stage 2 fell back to FP16");
        ExpectShift(2, "mixed cascade");
        Check(s.frameColourCopies == 4, "the mixed frame is four colour copies (the chained prefix's copy-in and copy-home, the FP16 stage's own pair)");
        Check(s.frameCrossApiTrips == 2, "the mixed frame is two cross-API returns (the prefix's, the FP16 stage's)");
        Check(!Platform::SidecarFrame::Snapshot().hudlessLive,
            "nothing is published when an FP16 stage ends the frame: the prefix's image must not outlive its invalidation (RT0 holds the FP16 result)");
    }
    DrainDebugMessages("mixed cascade");

    {
        const auto s = Platform::NeuralPass::Snapshot();
        Check(s.d3d11Timed, "the D3D11 transfer brackets armed on the fixture's device");
        Check(s.d3d11PrepBrackets > 0, "the PREPARATION lane retired brackets across the scenarios");
        Check(s.d3d11ReturnBrackets > 0, "the RETURN lane retired brackets across the scenarios");
        constexpr std::uint64_t kExpectedBytes = 32768ULL + 32768ULL + 65536ULL + 32768ULL;
        Check(s.frameCopyBytes == kExpectedBytes, "the mixed frame's copied bytes are exactly 163840 (three native copies + one FP16 copy-in at 128x64)");
        if (s.frameCopyBytes != kExpectedBytes) std::printf("    (read %llu bytes)\n", static_cast<unsigned long long>(s.frameCopyBytes));
    }

    Platform::NeuralRendererStub::RefuseNativeCarrier(Platform::NeuralRendererStub::kNoFailure);
    settings.neural.enabled = true;
    settings.neural.passCount = 1;
    Platform::NeuralRendererStub::SetIdentity(true);
    Platform::NeuralPass::ApplyMenuSettings(settings);
    Check(RunUntilDelivered(1, 60), "the one-stage identity cascade delivers at 100 %");
    ExpectOffset(0.0, 0, 0.0, "identity at 100 %");
    for (const std::uint32_t percent : { 50U, 75U, 125U, 150U, 200U, 300U }) {
        char label[64]{};
        std::snprintf(label, sizeof(label), "identity at %u %%", percent);
        char message[160]{};
        std::snprintf(message, sizeof(message), "%s: the cascade re-creates and delivers at the new extent", label);
        Check(SetModelPercent(settings, percent, 1), message);
        const auto s = Platform::NeuralPass::Snapshot();
        const auto extent = Platform::Neural::ComputeModelExtent(kWidth, kHeight, percent);
        std::snprintf(message, sizeof(message), "%s: the effective model extent is %ux%u", label, extent.width, extent.height);
        Check(s.modelWidth == extent.width && s.modelHeight == extent.height && s.modelReason[0] == '\0', message);
        if (s.modelReason[0] != '\0') std::printf("    (reason: %s)\n", s.modelReason);
        ExpectOffset(0.0, 0, 0.0, label);
        Check(Platform::SidecarFrame::Snapshot().hudlessLive, "the resized cascade published its full-resolution HUD-less image");
        Check(s.frameColourCopies == 2 && s.frameCrossApiTrips == 1, "the two copies and one return hold at a resized extent (the kernels ride the sidecar list)");
        std::snprintf(message, sizeof(message), "%s: the stub created its feature at the model's extent", label);
        Check(Platform::NeuralRenderer::CurrentKey(0).outWidth == extent.width && Platform::NeuralRenderer::CurrentKey(0).outHeight == extent.height, message);
        DrainDebugMessages(label);
    }
    g_alternating = true;
    Check(SetModelPercent(settings, 50, 1), "the identity cascade delivers the alternating base at 50 %");
    ExpectOffset(0.0, 0, 0.0, "identity at 50 % on alternating columns");
    std::printf("  naive control (derived, not executed): reduce-then-enlarge of the alternating base loses all %u columns; the residual path lost none\n", kWidth);
    g_alternating = false;
    DrainDebugMessages("identity on alternating columns");
    Platform::NeuralRendererStub::SetIdentity(false);
    Check(SetModelPercent(settings, 100, 1), "back to 100 % with the shift model");
    ExpectShift(1, "shift at 100 %");
    Check(SetModelPercent(settings, 50, 1), "the shift cascade delivers at 50 %");
    ExpectOffset(-4.0, 8, 1.0, "shift at 50 %: the residual is the base shifted by two pixels (one reduced pixel)");
    settings.neural.passCount = 3;
    Platform::NeuralPass::ApplyMenuSettings(settings);
    Check(RunUntilDelivered(3, 60), "three chained stages deliver at 50 %");
    ExpectOffset(-12.0, 16, 1.0, "three shifts at 50 %: six pixels — the cumulative residual against the first stage's exact input");
    {
        const auto s = Platform::NeuralPass::Snapshot();
        Check(s.frameColourCopies == 2 && s.frameCrossApiTrips == 1, "three resized chained stages: still two copies and one return");
    }
    DrainDebugMessages("shift at 50 %");
    Check(SetModelPercent(settings, 150, 3), "three chained stages deliver at 150 %");
    ExpectOffset(-4.0, 16, 1.0, "three shifts at 150 %: four pixels (one model pixel is two thirds of a base pixel)");
    DrainDebugMessages("shift at 150 %");
    Check(SetModelPercent(settings, 300, 3), "three chained stages deliver at 300 %");
    ExpectOffset(-2.0, 16, 1.0, "three shifts at 300 %: two pixels (one model pixel is a third of a base pixel; the four-tap reduction)");
    DrainDebugMessages("shift at 300 %");
    settings.neural.passCount = 1;
    Check(SetModelPercent(settings, 100, 1), "back to the exact path");
    ExpectShift(1, "shift at 100 % again");
    DrainDebugMessages("the model extent");

    settings.neural.enabled = false;
    Platform::NeuralPass::ApplyMenuSettings(settings);
    for (int i = 0; i < 6; ++i) RunFrame();
    Check(!Platform::NeuralPass::Snapshot().enabled, "the cascade is off after the disable commit");
    ExpectRetired("teardown");
    DrainDebugMessages("teardown");

    Platform::NeuralRendererStub::RefuseNativeCarrier(Platform::NeuralRendererStub::kNoFailure);
    Platform::NeuralRendererStub::SetIdentity(false);
    settings.neural.enabled = true;
    settings.neural.passCount = 1;
    settings.neural.modelPercent = 100;
    Platform::NeuralPass::ApplyMenuSettings(settings);
    Check(RunUntilDelivered(1, 60), "a one-stage cascade delivers before the stall");
    {
        Check(Platform::D3D12Sidecar::ProveRetired(), "the queue is idle before the stall is staged");
        ComPtr<ID3D12Fence> block;
        ID3D12Device* const device12 = Platform::D3D12Sidecar::Device();
        ID3D12CommandQueue* const queue = Platform::D3D12Sidecar::Queue();
        const bool staged = device12 != nullptr && queue != nullptr &&
            SUCCEEDED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&block))) && block &&
            SUCCEEDED(queue->Wait(block.Get(), 1));
        Check(staged, "the stall is staged (the sidecar queue waits on the fixture's own fence)");
        std::printf("  stall staged\n");
        RunFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        RunFrame();
        Check(Platform::D3D12Sidecar::Disabled(), "the 500 ms rule disabled the session");
        Check(Platform::D3D12Sidecar::ForcedValue() != 0, "the forced F12 value is recorded");
        Check(Platform::D3D12Sidecar::Quarantined(), "a forced value on a live device = quarantined");
        Check(!Platform::D3D12Sidecar::DeviceRemoved(), "the device is alive (a stall, not a loss)");
        std::printf("  forced F12 value %llu; %s\n", static_cast<unsigned long long>(Platform::D3D12Sidecar::ForcedValue()),
            Platform::D3D12Sidecar::DisableReason());
        const std::uint32_t releasesBefore = Platform::NeuralRendererStub::Releases(0);
        const std::uint32_t abandonsBefore = Platform::NeuralRendererStub::Abandons(0);
        settings.neural.enabled = false;
        Platform::NeuralPass::ApplyMenuSettings(settings);
        for (int i = 0; i < 3; ++i) RunFrame();
        ExpectRetired("the abandoned stage reports no feature and no ready pass");
        Check(Platform::NeuralRendererStub::Abandons(0) == abandonsBefore + 1U && Platform::NeuralRendererStub::Releases(0) == releasesBefore,
            "the stage's feature was ABANDONED, never released (the stub counts the two apart; a release under the GPU would move the other count)");
        Check(!Platform::NeuralPass::Snapshot().enabled, "the disable commit completed (destroyed, nothing freed)");
        Platform::NeuralPass::RequestRetry();
        RunFrame();
        Check(Platform::D3D12Sidecar::Disabled() && Platform::D3D12Sidecar::Quarantined(), "a retry without the proof is refused; still quarantined");
        Check(staged && SUCCEEDED(block->Signal(1)), "the fixture releases the queue");
        {
            const auto before = Platform::D3D12Sidecar::HealthSnapshot();
            const bool moved = Platform::D3D12Sidecar::WaitForValue(before.submitted, 5000);
            const auto after = Platform::D3D12Sidecar::HealthSnapshot();
            std::printf("  after the release the queue %s (F12 completed %llu of %llu)\n", moved ? "ran" : "did NOT run within 5 s",
                static_cast<unsigned long long>(after.completed), static_cast<unsigned long long>(after.submitted));
            Check(moved, "the released queue ran the stalled work to completion");
        }
        Platform::NeuralPass::RequestRetry();
        RunFrame();
        Check(!Platform::D3D12Sidecar::Disabled(), "after the queue ran, the retry proved retirement and re-armed the session");
        Check(Platform::D3D12Sidecar::ForcedValue() == 0 && !Platform::D3D12Sidecar::Quarantined(), "the forced value is void after the proof");
        settings.neural.enabled = true;
        Platform::NeuralPass::ApplyMenuSettings(settings);
        Check(RunUntilDelivered(1, 60), "the pass rebuilds and delivers again after the proof");
        ExpectShift(1, "delivered after the recovery");
    }
    DrainDebugMessages("the forced release and the proof");
    settings.neural.enabled = false;
    Platform::NeuralPass::ApplyMenuSettings(settings);
    for (int i = 0; i < 3; ++i) RunFrame();

    Platform::SidecarCompute::Release();
    Platform::D3D12Sidecar::Close();

    if (g_failures != 0) {
        std::printf("NeuralCascadeGpuTests: %d failure(s) of %d\n", g_failures, g_checks);
        return EXIT_FAILURE;
    }
    std::printf("NeuralCascadeGpuTests: all checks passed (%d)\n", g_checks);
    return EXIT_SUCCESS;
}
