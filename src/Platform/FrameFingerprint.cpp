#include "PCH.h"

#include "Platform/FrameFingerprint.h"

#include "Core/DeviceObjects.h"
#include "Platform/Fallout4Renderer.h"
#include "Platform/FrameBufferResolve.h"
#include "Platform/FrameGenEngine.h"
#include "Platform/NeuralPass.h"
#include "Platform/OutputMergerScope.h"
#include "Platform/SidecarGuides.h"
#include "Platform/Streamline.h"
#include "RE/Bethesda/BSGraphics.h"
#include "UI/Menu.h"

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "CSFingerprint.h"

namespace Platform::FrameFingerprint
{
    namespace
    {
        using namespace Platform::SidecarGuides;
        using Fingerprint::kPoints;
        using Fingerprint::kSumsPerPoint;
        using Fingerprint::kSumsTotal;

        constexpr std::uint32_t kStagingSlots = 4;

        struct PointObjects
        {
            ID3D11Texture2D* prev{ nullptr };
            ID3D11ShaderResourceView* prevSrv{ nullptr };
            ID3D11ShaderResourceView* curSrv{ nullptr };
            ID3D11Texture2D* curSrvTexture{ nullptr };
            std::uint32_t width{ 0 }, height{ 0 };
            DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
            bool havePrev{ false };
            bool sampled{ false };
            bool failed{ false };
            DXGI_FORMAT failedFormat{ DXGI_FORMAT_UNKNOWN };
        };

        struct SlotMeta
        {
            bool pending{ false };
            std::uint64_t frame{ 0 };
            double timeMs{ 0.0 };
            bool sampled[kPoints]{};
            std::uint32_t width[kPoints]{}, height[kPoints]{};
            bool neuralOn{ false };
            std::uint32_t passesDelivered{ 0 };
            bool neuralReset{ false };
            std::uint64_t historyEpoch{ 0 };
            int guidesSource{ -1 };
            bool fgInterpolating{ false };
            bool menuOpen{ false };
            bool aaDrain{ false };
            float neuralCpuMs{ 0.0F };
        };

        struct Tool
        {
            std::atomic<bool> wantEnabled{ false };
            std::atomic<bool> markRequested{ false };
            bool enabled{ false };
            ID3D11Device* device{ nullptr };
            ID3D11ComputeShader* cs{ nullptr };
            ID3D11Buffer* params{ nullptr };
            ID3D11Buffer* sums{ nullptr };
            ID3D11UnorderedAccessView* sumsUav{ nullptr };
            ID3D11Buffer* staging[kStagingSlots]{};
            SlotMeta meta[kStagingSlots]{};
            std::uint32_t slot{ 0 };
            PointObjects points[kPoints]{};
            bool frameOpen{ false };
            LONGLONG qpf{ 0 }, t0{ 0 };
            Fingerprint::FrameRecord ring[kRingFrames]{};
            std::uint32_t ringCount{ 0 };
            std::uint32_t ringHead{ 0 };
            bool markArmed{ false };
            std::uint32_t markCountdown{ 0 };
            std::uint64_t markFrame{ 0 };
            std::uint32_t dumps{ 0 };
            std::uint32_t droppedReads{ 0 };
            std::atomic<float> snapDelta[kPoints]{};
            std::atomic<bool> snapValid[kPoints]{};
            std::atomic<std::uint32_t> snapRing{ 0 };
            std::atomic<std::uint32_t> snapDumps{ 0 };
            std::atomic<std::uint32_t> snapDropped{ 0 };
            std::atomic<bool> snapAllocated{ false };
            std::atomic<bool> snapMarkArmed{ false };
            std::atomic<std::uint32_t> snapCountdown{ 0 };
            char lastVerdict[240]{};
            char lastDump[260]{};
            char reason[160]{};
            std::mutex textMutex;
        };
        Tool g;

        template <class T>
        void SafeRelease(T*& a_object) noexcept
        {
            if (a_object != nullptr) {
                a_object->Release();
                a_object = nullptr;
            }
        }

        void SetReason(const char* a_reason) noexcept
        {
            const std::scoped_lock lock(g.textMutex);
            std::snprintf(g.reason, sizeof(g.reason), "%s", a_reason != nullptr ? a_reason : "");
        }

        [[nodiscard]] std::string NarrowPath(const std::filesystem::path& a_path) noexcept
        {
            try {
                const std::u8string utf8 = a_path.u8string();
                return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            } catch (...) {
                return "(a path this build cannot print)";
            }
        }

        void ReleasePoint(PointObjects& p) noexcept
        {
            SafeRelease(p.prevSrv);
            SafeRelease(p.prev);
            SafeRelease(p.curSrv);
            p.curSrvTexture = nullptr;
            p.width = p.height = 0;
            p.format = DXGI_FORMAT_UNKNOWN;
            p.havePrev = false;
            p.sampled = false;
            p.failed = false;
            p.failedFormat = DXGI_FORMAT_UNKNOWN;
        }

        void Release() noexcept
        {
            for (auto& p : g.points) ReleasePoint(p);
            for (auto& s : g.staging) SafeRelease(s);
            for (auto& m : g.meta) m = SlotMeta{};
            SafeRelease(g.sumsUav);
            SafeRelease(g.sums);
            SafeRelease(g.params);
            SafeRelease(g.cs);
            g.device = nullptr;
            g.frameOpen = false;
            g.slot = 0;
            g.snapAllocated.store(false, std::memory_order_relaxed);
            for (std::uint32_t p = 0; p < kPoints; ++p) g.snapValid[p].store(false, std::memory_order_relaxed);
        }

        [[nodiscard]] bool Allocate(ID3D11Device* a_device) noexcept
        {
            Release();
            if (FAILED(a_device->CreateComputeShader(g_csFingerprint, sizeof(g_csFingerprint), nullptr, &g.cs))) {
                SetReason("the fingerprint shader would not create");
                return false;
            }
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 32;
            cb.Usage = D3D11_USAGE_DEFAULT;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(a_device->CreateBuffer(&cb, nullptr, &g.params))) {
                SetReason("the fingerprint constants would not create");
                Release();
                return false;
            }
            D3D11_BUFFER_DESC sums{};
            sums.ByteWidth = kSumsTotal * sizeof(std::uint32_t);
            sums.Usage = D3D11_USAGE_DEFAULT;
            sums.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            sums.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            if (FAILED(a_device->CreateBuffer(&sums, nullptr, &g.sums))) {
                SetReason("the fingerprint sums buffer would not create");
                Release();
                return false;
            }
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = kSumsTotal;
            uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            if (FAILED(a_device->CreateUnorderedAccessView(g.sums, &uav, &g.sumsUav))) {
                SetReason("the fingerprint sums view would not create");
                Release();
                return false;
            }
            D3D11_BUFFER_DESC staging{};
            staging.ByteWidth = kSumsTotal * sizeof(std::uint32_t);
            staging.Usage = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& s : g.staging) {
                if (FAILED(a_device->CreateBuffer(&staging, nullptr, &s))) {
                    SetReason("the fingerprint readback ring would not create");
                    Release();
                    return false;
                }
            }
            g.device = a_device;
            LARGE_INTEGER f{}, t{};
            ::QueryPerformanceFrequency(&f);
            ::QueryPerformanceCounter(&t);
            g.qpf = f.QuadPart;
            g.t0 = t.QuadPart;
            g.ringCount = 0;
            g.ringHead = 0;
            g.snapRing.store(0, std::memory_order_relaxed);
            g.snapAllocated.store(true, std::memory_order_relaxed);
            SetReason("");
            logger::info("[Fingerprint] ON: five points per frame (neural input, neural output, the game buffer at Present, after the canvas, and entering the proxy after the limiter), 4x4 tiles, a ring of {} "
                         "frames; the mark hotkey (Hotkeys tab, optional) or the Debug button marks an incident (the dump follows {} frames later)",
                kRingFrames, kPostMarkFrames);
            return true;
        }

        [[nodiscard]] double NowMs() noexcept
        {
            LARGE_INTEGER t{};
            ::QueryPerformanceCounter(&t);
            return g.qpf != 0 ? 1000.0 * static_cast<double>(t.QuadPart - g.t0) / static_cast<double>(g.qpf) : 0.0;
        }

        void BeginFrameIfNeeded(ID3D11DeviceContext* a_context) noexcept
        {
            if (g.frameOpen) return;
            const UINT zero[4]{};
            a_context->ClearUnorderedAccessViewUint(g.sumsUav, zero);
            for (auto& p : g.points) p.sampled = false;
            g.frameOpen = true;
        }

        void Sample(Point a_point, ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_texture) noexcept
        {
            if (!g.enabled || a_texture == nullptr || a_device != g.device) return;
            auto& p = g.points[static_cast<std::uint32_t>(a_point)];
            D3D11_TEXTURE2D_DESC desc{};
            a_texture->GetDesc(&desc);
            if (desc.Width < 32U || desc.Height < 32U || desc.SampleDesc.Count != 1U) return;
            if (p.failed && desc.Format == p.failedFormat) return;
            BeginFrameIfNeeded(a_context);
            const auto latch = [&](const char* a_what) noexcept {
                p.failed = true;
                p.failedFormat = desc.Format;
                logger::warn("[Fingerprint] the {} point cannot be sampled: {} (fmt={}); it stays invalid until the format changes",
                    Fingerprint::PointName(a_point), a_what, static_cast<int>(desc.Format));
            };
            if (p.prev == nullptr || p.width != desc.Width || p.height != desc.Height || p.format != desc.Format) {
                SafeRelease(p.prevSrv);
                SafeRelease(p.prev);
                D3D11_TEXTURE2D_DESC keep = desc;
                keep.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                keep.MiscFlags = 0;
                keep.CPUAccessFlags = 0;
                keep.Usage = D3D11_USAGE_DEFAULT;
                if (FAILED(a_device->CreateTexture2D(&keep, nullptr, &p.prev))) {
                    p.prev = nullptr;
                    latch("its retained copy would not create");
                    return;
                }
                D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = TypedColorFormat(desc.Format);
                srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D.MipLevels = 1;
                if (FAILED(a_device->CreateShaderResourceView(p.prev, &srv, &p.prevSrv))) {
                    SafeRelease(p.prev);
                    latch("the retained copy's view would not create");
                    return;
                }
                p.width = desc.Width;
                p.height = desc.Height;
                p.format = desc.Format;
                p.havePrev = false;
            }
            ID3D11ShaderResourceView* cur = EnsureOwnSrv(a_device, a_texture, TypedColorFormat(desc.Format), p.curSrv, p.curSrvTexture);
            if (cur == nullptr) {
                latch("its own view on the source would not create");
                return;
            }
            if (p.havePrev) {
                const OutputMergerUnbindScope om(a_context);
                const ComputeBindingsScope cs(a_context);
                const std::uint32_t params[8]{ static_cast<std::uint32_t>(a_point), desc.Width, desc.Height,
                    (desc.Width + Fingerprint::kTilesX - 1U) / Fingerprint::kTilesX,
                    (desc.Height + Fingerprint::kTilesY - 1U) / Fingerprint::kTilesY, 0U, 0U, 0U };
                a_context->UpdateSubresource(g.params, 0, nullptr, params, 0, 0);
                a_context->CSSetShader(g.cs, nullptr, 0);
                a_context->CSSetConstantBuffers(0, 1, &g.params);
                ID3D11ShaderResourceView* srvs[]{ cur, p.prevSrv };
                a_context->CSSetShaderResources(0, 2, srvs);
                a_context->CSSetUnorderedAccessViews(0, 1, &g.sumsUav, nullptr);
                a_context->Dispatch((desc.Width + 7U) / 8U, (desc.Height + 7U) / 8U, 1U);
                p.sampled = true;
            }
            a_context->CopyResource(p.prev, a_texture);
            p.havePrev = true;
        }

        void PushRecord(const Fingerprint::FrameRecord& a_record) noexcept
        {
            g.ring[g.ringHead] = a_record;
            g.ringHead = (g.ringHead + 1U) % kRingFrames;
            if (g.ringCount < kRingFrames) ++g.ringCount;
            g.snapRing.store(g.ringCount, std::memory_order_relaxed);
            for (std::uint32_t p = 0; p < kPoints; ++p) {
                g.snapDelta[p].store(a_record.points[p].meanDelta, std::memory_order_relaxed);
                g.snapValid[p].store(a_record.points[p].valid, std::memory_order_relaxed);
            }
        }

        std::uint32_t Ordered(Fingerprint::FrameRecord* a_out) noexcept
        {
            const std::uint32_t n = g.ringCount;
            const std::uint32_t start = n < kRingFrames ? 0U : g.ringHead;
            for (std::uint32_t i = 0; i < n; ++i) a_out[i] = g.ring[(start + i) % kRingFrames];
            return n;
        }

        void DumpBody(bool a_captureOff)
        {
            static Fingerprint::FrameRecord ordered[kRingFrames];
            const std::uint32_t n = Ordered(ordered);
            if (n == 0U) return;
            if (a_captureOff) g.markFrame = ordered[n - 1U].frame;
            std::uint32_t markIndex = n;
            for (std::uint32_t i = 0; i < n; ++i) {
                if (ordered[i].frame >= g.markFrame) { markIndex = i; break; }
            }
            const std::uint32_t windowEnd = markIndex < n ? markIndex + 1U : n;
            const std::uint32_t windowStart = windowEnd > kPostMarkFrames ? windowEnd - kPostMarkFrames : 0U;
            const auto analysis = Fingerprint::Classify<kRingFrames>(ordered + windowStart, windowEnd - windowStart);

            ++g.dumps;
            std::filesystem::path path;
            if (auto dir = logger::log_directory(); dir) {
                SYSTEMTIME now{};
                ::GetLocalTime(&now);
                path = *dir / fmt::format("FO4GraphicsOverhaul-fingerprint-{:04}{:02}{:02}-{:02}{:02}{:02}-{}{}.csv",
                    now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, g.dumps, a_captureOff ? "-captureoff" : "");
            }
            static constexpr const char* kNames[kPoints]{ "A", "B", "C", "D", "E" };
            bool written = false;
            if (!path.empty()) {
                std::FILE* f = nullptr;
                if (::_wfopen_s(&f, path.c_str(), L"w") == 0 && f != nullptr) {
                    std::fputs("frame,time_ms,marked,neural_on,passes,reset,epoch,guides,fg_interp_prev,menu,aa_drain,neural_cpu_ms", f);
                    for (const char* name : kNames) std::fprintf(f, ",%s_luma,%s_delta,%s_maxtile", name, name, name);
                    for (const char* name : kNames) {
                        for (std::uint32_t t = 0; t < Fingerprint::kTiles; ++t) std::fprintf(f, ",%s_d%u", name, t);
                    }
                    std::fputc('\n', f);
                    for (std::uint32_t i = 0; i < n; ++i) {
                        const auto& r = ordered[i];
                        std::fprintf(f, "%llu,%.2f,%d,%d,%u,%d,%llu,%d,%d,%d,%d,%.3f", static_cast<unsigned long long>(r.frame),
                            r.timeMs, i == markIndex ? 1 : 0, r.neuralOn ? 1 : 0, r.passesDelivered,
                            r.neuralReset ? 1 : 0, static_cast<unsigned long long>(r.historyEpoch), r.guidesSource,
                            r.fgInterpolating ? 1 : 0, r.menuOpen ? 1 : 0, r.aaDrain ? 1 : 0, static_cast<double>(r.neuralCpuMs));
                        for (const auto& s : r.points) {
                            if (s.valid) {
                                std::fprintf(f, ",%.2f,%.3f,%.3f", static_cast<double>(s.meanLuma), static_cast<double>(s.meanDelta),
                                    static_cast<double>(s.maxTileDelta));
                            } else {
                                std::fputs(",,,", f);
                            }
                        }
                        for (const auto& s : r.points) {
                            for (std::uint32_t t = 0; t < Fingerprint::kTiles; ++t) {
                                if (s.valid) std::fprintf(f, ",%.3f", static_cast<double>(s.tileDelta[t]));
                                else std::fputs(",", f);
                            }
                        }
                        std::fputc('\n', f);
                    }
                    std::fclose(f);
                    written = true;
                }
            }
            {
                const std::scoped_lock lock(g.textMutex);
                std::snprintf(g.lastVerdict, sizeof(g.lastVerdict), "%s", Fingerprint::VerdictName(analysis.verdict));
                std::snprintf(g.lastDump, sizeof(g.lastDump), "%s", written ? NarrowPath(path).c_str() : "(the CSV could not be written)");
            }
            g.snapDumps.store(g.dumps, std::memory_order_relaxed);
            std::string points;
            for (std::uint32_t p = 0; p < kPoints; ++p) {
                points += fmt::format(" | {}: mean median {:.2f} / max {:.2f} at frame {}, tile median {:.2f} / max {:.2f} at frame {}{}",
                    Fingerprint::PointName(static_cast<Point>(p)), analysis.baseline[p], analysis.spike[p], analysis.spikeFrame[p],
                    analysis.baselineTile[p], analysis.spikeTile[p], analysis.spikeTileFrame[p], analysis.spiked[p] ? " SPIKE" : "");
            }
            logger::info("[Fingerprint] {} #{}: {} frames dumped to {} | window of {} frames before the mark (frame {}){} | handoff black frames {} | verdict: {}",
                a_captureOff ? "capture-off snapshot" : "incident", g.dumps, n, written ? NarrowPath(path) : std::string("(not written)"), analysis.frames, g.markFrame, points,
                analysis.handoffBlackFrames, Fingerprint::VerdictName(analysis.verdict));
        }

        void Dump(bool a_captureOff = false) noexcept
        {
            try {
                DumpBody(a_captureOff);
            } catch (...) {
                try {
                    SetReason("capture dump could not finish (allocation/filesystem error)");
                    logger::error("[Fingerprint] capture dump failed; the game continues");
                } catch (...) {
                    ::OutputDebugStringA("[FO4GraphicsOverhaul] fingerprint dump failed\n");
                }
            }
        }

        void CloseFrame(ID3D11DeviceContext* a_context) noexcept
        {
            const std::uint64_t frame = Fallout4Renderer::FrameStampNow();
            if (g.frameOpen) {
                auto& m = g.meta[g.slot];
                if (m.pending) {
                    ++g.droppedReads;
                    g.snapDropped.store(g.droppedReads, std::memory_order_relaxed);
                }
                a_context->CopyResource(g.staging[g.slot], g.sums);
                m = SlotMeta{};
                m.pending = true;
                m.frame = frame;
                m.timeMs = NowMs();
                for (std::uint32_t p = 0; p < kPoints; ++p) {
                    m.sampled[p] = g.points[p].sampled;
                    m.width[p] = g.points[p].width;
                    m.height[p] = g.points[p].height;
                }
                const auto neural = NeuralPass::Snapshot();
                m.neuralOn = neural.enabled;
                m.passesDelivered = neural.activePasses;
                m.neuralReset = NeuralPass::LastResetFlag();
                m.neuralCpuMs = neural.cpuMs;
                m.guidesSource = neural.guidesSource;
                m.historyEpoch = Fallout4Renderer::HistoryEpoch();
                m.fgInterpolating = FrameGenEngine::Interpolating();
                m.menuOpen = UI::Menu::GetSingleton().IsOpen();
                m.aaDrain = Streamline::DrainAffectsActiveEngine();
                g.slot = (g.slot + 1U) % kStagingSlots;
                g.frameOpen = false;
            }
            for (std::uint32_t k = 0; k < kStagingSlots; ++k) {
                const std::uint32_t s = (g.slot + k) % kStagingSlots;
                auto& m = g.meta[s];
                if (!m.pending) continue;
                D3D11_MAPPED_SUBRESOURCE mapped{};
                const HRESULT hr = a_context->Map(g.staging[s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
                if (FAILED(hr) || mapped.pData == nullptr) {
                    m.pending = false;
                    ++g.droppedReads;
                    g.snapDropped.store(g.droppedReads, std::memory_order_relaxed);
                    continue;
                }
                std::uint32_t sums[kSumsTotal]{};
                std::memcpy(sums, mapped.pData, sizeof(sums));
                a_context->Unmap(g.staging[s], 0);
                Fingerprint::FrameRecord r{};
                r.frame = m.frame;
                r.timeMs = m.timeMs;
                for (std::uint32_t p = 0; p < kPoints; ++p) {
                    if (m.sampled[p]) r.points[p] = Fingerprint::FromSums(sums + p * kSumsPerPoint, m.width[p], m.height[p]);
                }
                r.neuralOn = m.neuralOn;
                r.passesDelivered = m.passesDelivered;
                r.neuralReset = m.neuralReset;
                r.historyEpoch = m.historyEpoch;
                r.guidesSource = m.guidesSource;
                r.fgInterpolating = m.fgInterpolating;
                r.menuOpen = m.menuOpen;
                r.aaDrain = m.aaDrain;
                r.neuralCpuMs = m.neuralCpuMs;
                m.pending = false;
                PushRecord(r);
            }
            if (g.markRequested.exchange(false, std::memory_order_relaxed) && !g.markArmed) {
                g.markArmed = true;
                g.markCountdown = kPostMarkFrames;
                g.markFrame = frame;
                logger::info("[Fingerprint] incident marked at frame {}; the ring records {} more frames, then dumps", frame, kPostMarkFrames);
            }
            if (g.markArmed) {
                if (g.markCountdown != 0U) --g.markCountdown;
                if (g.markCountdown == 0U) {
                    g.markArmed = false;
                    Dump();
                }
            }
            g.snapMarkArmed.store(g.markArmed, std::memory_order_relaxed);
            g.snapCountdown.store(g.markCountdown, std::memory_order_relaxed);
        }

        void Reconcile(const Core::DeviceObjects::Snapshot& a_snap, IDXGISwapChain* a_swapChain) noexcept
        {
            const bool want = g.wantEnabled.load(std::memory_order_relaxed);
            if (g.enabled && (a_snap.device != g.device || !want)) {
                if (!want) Dump(true);
                g.markArmed = false;
                g.markCountdown = 0;
                g.markRequested.store(false, std::memory_order_relaxed);
                const std::uint32_t frames = g.ringCount, dumps = g.dumps;
                Release();
                g.enabled = false;
                if (!want) {
                    logger::info("[Fingerprint] OFF: released ({} frames were in the ring, {} dump(s))", frames, dumps);
                }
            }
            if (want && !g.enabled && a_snap.IsValid() && a_snap.swapChain == a_swapChain) {
                g.enabled = Allocate(a_snap.device);
                if (!g.enabled) {
                    g.wantEnabled.store(false, std::memory_order_relaxed);
                    logger::warn("[Fingerprint] could not start: {}", g.reason);
                }
            }
        }

        void SampleBackbuffer(Point a_point, const Core::DeviceObjects::Snapshot& a_snap, IDXGISwapChain* a_swapChain) noexcept
        {
            ID3D11Texture2D* backbuffer = nullptr;
            if (SUCCEEDED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) && backbuffer != nullptr) {
                Sample(a_point, a_snap.device, a_snap.context, backbuffer);
                backbuffer->Release();
            }
        }
    }

    void SetEnabled(bool a_on) noexcept
    {
        g.wantEnabled.store(a_on, std::memory_order_relaxed);
    }

    bool Enabled() noexcept
    {
        return g.wantEnabled.load(std::memory_order_relaxed);
    }

    void MarkIncident() noexcept
    {
        g.markRequested.store(true, std::memory_order_relaxed);
    }

    void TripOff(const char* a_reason) noexcept
    {
        g.wantEnabled.store(false, std::memory_order_relaxed);
        g.enabled = false;
        g.markArmed = false;
        g.markCountdown = 0;
        g.markRequested.store(false, std::memory_order_relaxed);
        SetReason(a_reason);
        logger::error("[Fingerprint] OFF after a fault: {} (nothing released; the tool stays off for this session)",
            a_reason != nullptr ? a_reason : "");
    }

    void SampleSeam(Point a_point) noexcept
    {
        if (!g.enabled || !g.wantEnabled.load(std::memory_order_relaxed)) return;
        try {
            auto* const data = RE::BSGraphics::RendererData::GetSingleton();
            if (data == nullptr || data->context == nullptr || data->device == nullptr) return;
            ResolvedFrameBuffer frame{};
            ResolveFrameBufferTexture(data->renderTargets[0], frame);
            if (frame.texture == nullptr) return;
            Sample(a_point, reinterpret_cast<ID3D11Device*>(data->device), data->context, frame.texture);
        } catch (...) {
        }
    }

    void SamplePresented(IDXGISwapChain* a_swapChain) noexcept
    {
        try {
            if (!g.wantEnabled.load(std::memory_order_relaxed) && !g.enabled) return;
            const auto snap = Core::DeviceObjects::Current();
            Reconcile(snap, a_swapChain);
            if (!g.enabled || !snap.IsValid() || snap.swapChain != a_swapChain) return;
            SampleBackbuffer(Point::kPresented, snap, a_swapChain);
        } catch (...) {
        }
    }

    void SampleCanvas(IDXGISwapChain* a_swapChain) noexcept
    {
        try {
            if (!g.wantEnabled.load(std::memory_order_relaxed) && !g.enabled) return;
            const auto snap = Core::DeviceObjects::Current();
            Reconcile(snap, a_swapChain);
            if (!g.enabled || !snap.IsValid() || snap.swapChain != a_swapChain) return;
            SampleBackbuffer(Point::kDisplayed, snap, a_swapChain);
        } catch (...) {
        }
    }

    static void EndFrameGuarded(IDXGISwapChain* a_swapChain) noexcept
    {
        try {
            if (!g.wantEnabled.load(std::memory_order_relaxed) && !g.enabled) return;
            const auto snap = Core::DeviceObjects::Current();
            Reconcile(snap, a_swapChain);
            if (!g.enabled || !snap.IsValid() || snap.swapChain != a_swapChain) return;
            SampleBackbuffer(Point::kProxyInput, snap, a_swapChain);
            CloseFrame(snap.context);
        } catch (...) {
        }
    }

    void EndFrame(IDXGISwapChain* a_swapChain) noexcept
    {
        __try {
            EndFrameGuarded(a_swapChain);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            TripOff("a hardware fault at the presentation handoff");
        }
    }

    State Snapshot() noexcept
    {
        State s{};
        s.enabled = g.wantEnabled.load(std::memory_order_relaxed);
        s.allocated = g.snapAllocated.load(std::memory_order_relaxed);
        for (std::uint32_t p = 0; p < kPoints; ++p) {
            s.lastDelta[p] = g.snapDelta[p].load(std::memory_order_relaxed);
            s.lastValid[p] = g.snapValid[p].load(std::memory_order_relaxed);
        }
        s.ringFrames = g.snapRing.load(std::memory_order_relaxed);
        s.dumps = g.snapDumps.load(std::memory_order_relaxed);
        s.droppedReads = g.snapDropped.load(std::memory_order_relaxed);
        s.markArmed = g.snapMarkArmed.load(std::memory_order_relaxed);
        s.markCountdown = g.snapCountdown.load(std::memory_order_relaxed);
        const std::scoped_lock lock(g.textMutex);
        std::memcpy(s.lastVerdict, g.lastVerdict, sizeof(s.lastVerdict));
        std::memcpy(s.lastDump, g.lastDump, sizeof(s.lastDump));
        std::memcpy(s.reason, g.reason, sizeof(s.reason));
        return s;
    }
}
