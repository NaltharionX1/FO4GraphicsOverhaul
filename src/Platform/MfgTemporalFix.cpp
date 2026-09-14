// SPDX-License-Identifier: GPL-3.0-or-later
// Portions ported from MFGAdaUnlock-RenoDx, Copyright (c) 2026 ImDreamt, MIT License.

#include "Platform/MfgTemporalFix.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr std::uint32_t kFatbinMagic = 0xBA55ED50U;
    constexpr std::size_t kOuterHeader = 16;
    constexpr std::uint32_t kPtxKind = 1;
    constexpr std::uint32_t kAdaArch = 89;
    constexpr std::uint64_t kUncompressedFlags = 0x41;
    constexpr std::size_t kExpectedMidpoints = 104;

    constexpr char kJoinLabel[] = "$L__BB0_3:";
    constexpr char kMidpointBits[] = "0f3F000000";
    constexpr char kMulPrefix[] = "mul.ftz.f32 ";
    constexpr char kCurrToPrev[] = "%f136";
    constexpr char kPrevToCurr[] = "%f134";
    constexpr std::size_t kScaleLength = 5;

    using Platform::MfgTemporalFix::Program;
    using Platform::MfgTemporalFix::Role;
    using Platform::MfgTemporalFix::kRoleCount;

    struct KernelProfile
    {
        Role role;
        const char* entryName;
        const char* descriptorName;
        std::ptrdiff_t entryNameOffset;
        std::ptrdiff_t descriptorNameOffset;
        std::size_t adaPtxBytes;
        std::size_t blackwellPtxBytes;
        std::uint32_t paramBytes;
        std::uint32_t sharedBytes;
    };

    constexpr KernelProfile kKernelProfiles[]{
        { Role::kMotionVector, "Kernel_EstimateIntermMvecsScatter", "EstimateIntermMvecsScatter", 0x10, -0x08, 99626, 90732, 144, 7776 },
        { Role::kInpaint, "Kernel_Prev2CurrUnpackPull", "Prev2CurrUnpackPull", 0x10, -0x08, 33410, 26440, 224, 3920 },
        { Role::kInpaintDecision, "Kernel_OutputPull", "OutputPull", 0x10, -0x08, 31304, 23117, 152, 784 },
    };

    constexpr std::uint32_t kBlackwellArch = 120;
    constexpr char kTargetBlackwell[] = ".target sm_120";
    constexpr char kTargetAda[] = ".target sm_89";

    [[nodiscard]] std::uint16_t ReadU16(const std::uint8_t* a_p) noexcept
    {
        std::uint16_t v = 0;
        std::memcpy(&v, a_p, sizeof(v));
        return v;
    }

    [[nodiscard]] std::uint32_t ReadU32(const std::uint8_t* a_p) noexcept
    {
        std::uint32_t v = 0;
        std::memcpy(&v, a_p, sizeof(v));
        return v;
    }

    [[nodiscard]] std::uint64_t ReadU64(const std::uint8_t* a_p) noexcept
    {
        std::uint64_t v = 0;
        std::memcpy(&v, a_p, sizeof(v));
        return v;
    }

    void Say(char* a_out, std::size_t a_size, const char* a_text) noexcept
    {
        std::snprintf(a_out, a_size, "%s", a_text);
    }

    [[nodiscard]] bool Lz4BlockDecompress(const std::uint8_t* a_src, std::size_t a_srcSize,
        std::uint8_t* a_dst, std::size_t a_dstSize) noexcept
    {
        std::size_t in = 0;
        std::size_t out = 0;
        while (in < a_srcSize) {
            const std::uint8_t token = a_src[in++];
            std::size_t literals = static_cast<std::size_t>(token >> 4);
            if (literals == 15) {
                std::uint8_t ext = 0;
                do {
                    if (in >= a_srcSize) {
                        return false;
                    }
                    ext = a_src[in++];
                    literals += ext;
                } while (ext == 0xFF);
            }
            if (literals > a_srcSize - in || literals > a_dstSize - out) {
                return false;
            }
            std::memcpy(a_dst + out, a_src + in, literals);
            in += literals;
            out += literals;
            if (in == a_srcSize) {
                break;
            }
            if (a_srcSize - in < 2) {
                return false;
            }
            const std::size_t back = static_cast<std::size_t>(a_src[in]) |
                                     (static_cast<std::size_t>(a_src[in + 1]) << 8);
            in += 2;
            if (back == 0 || back > out) {
                return false;
            }
            std::size_t match = 4 + static_cast<std::size_t>(token & 0x0F);
            if ((token & 0x0F) == 15) {
                std::uint8_t ext = 0;
                do {
                    if (in >= a_srcSize) {
                        return false;
                    }
                    ext = a_src[in++];
                    match += ext;
                } while (ext == 0xFF);
            }
            if (match > a_dstSize - out) {
                return false;
            }
            for (std::size_t i = 0; i < match; ++i) {
                a_dst[out + i] = a_dst[out + i - back];
            }
            out += match;
        }
        return in == a_srcSize && out == a_dstSize;
    }

    [[nodiscard]] bool FindPtxEntry(const std::uint8_t* a_fat, std::size_t a_fatSize, std::uint32_t a_arch,
        std::size_t& a_entryOffset) noexcept
    {
        if (a_fatSize < kOuterHeader || ReadU32(a_fat) != kFatbinMagic) {
            return false;
        }
        if (ReadU16(a_fat + 6) != kOuterHeader) {
            return false;
        }
        const std::uint64_t declared = ReadU64(a_fat + 8);
        if (declared > a_fatSize - kOuterHeader) {
            return false;
        }
        const std::size_t end = static_cast<std::size_t>(declared) + kOuterHeader;
        std::size_t p = kOuterHeader;
        while (p + 64 <= end) {
            const std::uint32_t kind = ReadU16(a_fat + p);
            const std::uint32_t hdr = ReadU32(a_fat + p + 4);
            const std::uint64_t payload = ReadU64(a_fat + p + 8);
            if (hdr < 64 || payload == 0) {
                return false;
            }
            if (p + hdr + payload > end) {
                return false;
            }
            if (kind == kPtxKind && ReadU32(a_fat + p + 28) == a_arch) {
                a_entryOffset = p;
                return true;
            }
            p += static_cast<std::size_t>(hdr) + static_cast<std::size_t>(payload);
        }
        return false;
    }

    [[nodiscard]] bool FindAdaPtxEntry(const std::uint8_t* a_fat, std::size_t a_fatSize, std::size_t& a_entryOffset) noexcept
    {
        return FindPtxEntry(a_fat, a_fatSize, kAdaArch, a_entryOffset);
    }

    [[nodiscard]] const KernelProfile* MatchKernelProfile(const std::uint8_t* a_fat, std::size_t a_fatSize, Role a_role) noexcept
    {
        std::size_t entry = 0;
        if (!FindAdaPtxEntry(a_fat, a_fatSize, entry)) {
            return nullptr;
        }
        const std::uint64_t raw = ReadU64(a_fat + entry + 56);
        for (const auto& profile : kKernelProfiles) {
            if (profile.role == a_role && raw == profile.adaPtxBytes) {
                return &profile;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool PointsToCString(const std::uint8_t* a_base, std::size_t a_imageSize,
        std::uint64_t a_value, const char* a_expected) noexcept
    {
        const auto start = reinterpret_cast<std::uintptr_t>(a_base);
        if (a_value < start || a_value >= start + a_imageSize) {
            return false;
        }
        const char* const s = reinterpret_cast<const char*>(a_value);
        const std::size_t length = std::strlen(a_expected);
        if (a_value + length + 1 > start + a_imageSize) {
            return false;
        }
        return std::memcmp(s, a_expected, length + 1) == 0;
    }

    [[nodiscard]] bool ReadRelativePointer(const std::uint8_t* a_base, std::size_t a_imageSize,
        const std::uint8_t* a_slot, std::ptrdiff_t a_displacement, std::uint64_t& a_value) noexcept
    {
        const auto start = reinterpret_cast<std::uintptr_t>(a_base);
        const std::uintptr_t end = start + a_imageSize;
        const auto address = reinterpret_cast<std::uintptr_t>(a_slot);
        std::uintptr_t field = address;
        if (a_displacement >= 0) {
            const auto distance = static_cast<std::uintptr_t>(a_displacement);
            if (distance > UINTPTR_MAX - address) {
                return false;
            }
            field = address + distance;
        } else {
            const auto distance = static_cast<std::uintptr_t>(-(a_displacement + 1)) + 1;
            if (distance > address) {
                return false;
            }
            field = address - distance;
        }
        if (field < start || field > end || sizeof(a_value) > end - field) {
            return false;
        }
        std::memcpy(&a_value, reinterpret_cast<const void*>(field), sizeof(a_value));
        return true;
    }

    [[nodiscard]] bool VerifyContainerEnd(const std::vector<std::uint8_t>& a_out, std::size_t a_originalSize, std::size_t a_entry,
        std::uint32_t a_hdr, std::uint64_t a_payload, char* a_why, std::size_t a_whySize) noexcept
    {
        const std::size_t declared = static_cast<std::size_t>(ReadU64(a_out.data() + 8)) + kOuterHeader;
        if (a_entry + a_hdr + a_payload != declared) {
            Say(a_why, a_whySize, "the rebuilt fatbin does not end at its PTX entry");
            return false;
        }
        if (a_out.size() < declared || a_out.size() < a_originalSize) {
            Say(a_why, a_whySize, "the rebuilt buffer is smaller than the container or than the original");
            return false;
        }
        for (std::size_t i = declared; i < a_out.size(); ++i) {
            if (a_out[i] != 0) {
                Say(a_why, a_whySize, "the padding beyond the rebuilt container is not zero");
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool VerifyRebuild(const std::vector<std::uint8_t>& a_out, std::size_t a_originalSize, const std::string& a_parameterName,
        char* a_why, std::size_t a_whySize)
    {
        std::size_t entry = 0;
        if (!FindAdaPtxEntry(a_out.data(), a_out.size(), entry)) {
            Say(a_why, a_whySize, "the rebuilt fatbin does not re-parse");
            return false;
        }
        const std::uint32_t hdr = ReadU32(a_out.data() + entry + 4);
        const std::uint64_t payload = ReadU64(a_out.data() + entry + 8);
        if (ReadU32(a_out.data() + entry + 16) != 0 || ReadU64(a_out.data() + entry + 56) != 0 ||
            ReadU64(a_out.data() + entry + 40) != kUncompressedFlags) {
            Say(a_why, a_whySize, "the rebuilt PTX entry is not marked uncompressed");
            return false;
        }
        if (!VerifyContainerEnd(a_out, a_originalSize, entry, hdr, payload, a_why, a_whySize)) {
            return false;
        }
        const auto* const text = reinterpret_cast<const char*>(a_out.data() + entry + hdr);
        const std::string ptx(text, static_cast<std::size_t>(payload));
        const std::string injected = "sub.ftz.f32 %f136, %f135, %f134;";
        if (ptx.find("ld.param.f32 %f134, [" + a_parameterName + "+32];") == std::string::npos ||
            ptx.find(injected) == std::string::npos) {
            Say(a_why, a_whySize, "the rebuilt PTX does not carry the temporal input");
            return false;
        }
        const std::size_t midLength = sizeof(kMidpointBits) - 1;
        const std::size_t mulLength = sizeof(kMulPrefix) - 1;
        for (std::size_t i = 0; i + midLength < ptx.size(); ++i) {
            if (std::memcmp(ptx.data() + i, kMidpointBits, midLength) != 0 || ptx[i + midLength] != ';') {
                continue;
            }
            std::size_t line = i;
            while (line > 0 && ptx[line - 1] != '\n') {
                --line;
            }
            if (i - line >= mulLength && std::memcmp(ptx.data() + line, kMulPrefix, mulLength) == 0) {
                Say(a_why, a_whySize, "a midpoint multiply survived the rewrite");
                return false;
            }
        }

        std::size_t oneMinusT = 0;
        std::size_t justT = 0;
        for (std::size_t i = 0; i + kScaleLength <= ptx.size(); ++i) {
            const bool curr = std::memcmp(ptx.data() + i, kCurrToPrev, kScaleLength) == 0;
            const bool prev = std::memcmp(ptx.data() + i, kPrevToCurr, kScaleLength) == 0;
            if (!curr && !prev) {
                continue;
            }
            if (i + kScaleLength >= ptx.size() || ptx[i + kScaleLength] != ';') {
                continue;
            }
            std::size_t line = i;
            while (line > 0 && ptx[line - 1] != '\n') {
                --line;
            }
            if (i - line < mulLength || std::memcmp(ptx.data() + line, kMulPrefix, mulLength) != 0) {
                continue;
            }
            (curr ? oneMinusT : justT)++;
        }
        const std::size_t half = kExpectedMidpoints / 2;
        if (oneMinusT != half || justT != half) {
            std::snprintf(a_why, a_whySize,
                "the rewrite split %zu / %zu instead of %zu / %zu (current->previous / previous->current)",
                oneMinusT, justT, half, half);
            return false;
        }
        return true;
    }

    void EmitAdaPtxEntry(const std::uint8_t* a_fat, std::size_t a_fatSize, std::size_t a_entry, const std::vector<std::uint8_t>& a_text,
        std::vector<std::uint8_t>& a_out)
    {
        const std::uint32_t hdr = ReadU32(a_fat + a_entry + 4);
        const std::size_t padded = (a_text.size() + 7) & ~std::size_t{ 7 };
        const std::size_t finalSize = a_entry + hdr + padded;

        a_out.assign(a_fat, a_fat + a_entry + hdr);
        a_out.resize(finalSize > a_fatSize ? finalSize : a_fatSize, 0);
        std::memcpy(a_out.data() + a_entry + hdr, a_text.data(), a_text.size());

        const std::uint64_t payload64 = padded;
        const std::uint32_t zero32 = 0;
        const std::uint64_t zero64 = 0;
        std::memcpy(a_out.data() + a_entry + 8, &payload64, sizeof(payload64));
        std::memcpy(a_out.data() + a_entry + 16, &zero32, sizeof(zero32));
        std::memcpy(a_out.data() + a_entry + 40, &kUncompressedFlags, sizeof(kUncompressedFlags));
        std::memcpy(a_out.data() + a_entry + 56, &zero64, sizeof(zero64));
        const std::uint64_t outer = finalSize - kOuterHeader;
        std::memcpy(a_out.data() + 8, &outer, sizeof(outer));
    }

    [[nodiscard]] bool BuildTemporalFatbin(const std::uint8_t* a_fat, std::size_t a_fatSize,
        const KernelProfile& a_profile, std::vector<std::uint8_t>& a_out, std::uint32_t& a_constants,
        std::size_t& a_ptxBytes, char* a_why, std::size_t a_whySize)
    {
        std::size_t entry = 0;
        if (!FindAdaPtxEntry(a_fat, a_fatSize, entry)) {
            Say(a_why, a_whySize, "no sm_89 PTX entry in the kernel's fatbin");
            return false;
        }

        const std::uint32_t hdr = ReadU32(a_fat + entry + 4);
        const std::uint32_t compressed = ReadU32(a_fat + entry + 16);
        const std::uint64_t raw = ReadU64(a_fat + entry + 56);
        if (compressed == 0 || raw == 0 || raw > (8U << 20)) {
            Say(a_why, a_whySize, "the PTX entry is not compressed the way this correction expects");
            return false;
        }
        if (raw != a_profile.adaPtxBytes) {
            std::snprintf(a_why, a_whySize, "the PTX is %llu bytes, expected %zu",
                static_cast<unsigned long long>(raw), a_profile.adaPtxBytes);
            return false;
        }

        std::vector<std::uint8_t> ptx(static_cast<std::size_t>(raw));
        if (!Lz4BlockDecompress(a_fat + entry + hdr, compressed, ptx.data(), ptx.size())) {
            Say(a_why, a_whySize, "the PTX did not decompress");
            return false;
        }
        a_ptxBytes = ptx.size();

        const std::string parameterName = std::string(a_profile.entryName) + "_param_0";
        const std::string entrySignature = std::string(".entry ") + a_profile.entryName + "(";
        const std::string parameterSignature = ".param .align 8 .b8 " + parameterName + "[144]";
        const std::string text(reinterpret_cast<const char*>(ptx.data()), ptx.size());
        if (text.find(entrySignature) == std::string::npos ||
            text.find(parameterSignature) == std::string::npos ||
            text.find(".reg .f32 %f<1362>;") == std::string::npos) {
            Say(a_why, a_whySize, "the temporal kernel's signature changed (a different runtime build)");
            return false;
        }

        const char* const begin = reinterpret_cast<const char*>(ptx.data());
        const std::size_t n = ptx.size();
        const std::size_t labelLength = sizeof(kJoinLabel) - 1;
        std::size_t label = SIZE_MAX;
        for (std::size_t i = 0; i + labelLength <= n; ++i) {
            if (std::memcmp(begin + i, kJoinLabel, labelLength) != 0) {
                continue;
            }
            if (label != SIZE_MAX) {
                Say(a_why, a_whySize, "the join label is not unique");
                return false;
            }
            label = i;
        }
        if (label == SIZE_MAX) {
            Say(a_why, a_whySize, "the join label was not found");
            return false;
        }
        std::size_t insertion = label + labelLength;
        while (insertion < n && begin[insertion] != '\n') {
            ++insertion;
        }
        if (insertion >= n) {
            Say(a_why, a_whySize, "the join label has no line end");
            return false;
        }
        ++insertion;

        const std::size_t midLength = sizeof(kMidpointBits) - 1;
        const std::size_t mulLength = sizeof(kMulPrefix) - 1;
        std::vector<std::size_t> marks;
        marks.reserve(kExpectedMidpoints);
        for (std::size_t i = 0; i + midLength < n; ++i) {
            if (std::memcmp(begin + i, kMidpointBits, midLength) != 0) {
                continue;
            }
            if (begin[i + midLength] != ';') {
                continue;
            }
            std::size_t line = i;
            while (line > 0 && begin[line - 1] != '\n') {
                --line;
            }
            if (i - line < mulLength) {
                continue;
            }
            if (std::memcmp(begin + line, kMulPrefix, mulLength) != 0) {
                continue;
            }
            marks.push_back(i);
        }
        if (marks.size() != kExpectedMidpoints) {
            std::snprintf(a_why, a_whySize, "found %zu midpoint multiplies, expected %zu",
                marks.size(), kExpectedMidpoints);
            return false;
        }
        if (marks.front() <= insertion) {
            Say(a_why, a_whySize, "the first midpoint precedes the injection point");
            return false;
        }

        const std::string temporalInput =
            "ld.param.f32 %f134, [" + parameterName + "+32];\r\n"
            "mov.f32 %f135, 0f3F800000;\r\n"
            "sub.ftz.f32 %f136, %f135, %f134;\r\n";

        std::vector<std::uint8_t> patched;
        patched.reserve(n + temporalInput.size());
        const auto append = [&patched](const void* a_p, std::size_t a_bytes) {
            const auto* const b = static_cast<const std::uint8_t*>(a_p);
            patched.insert(patched.end(), b, b + a_bytes);
        };
        append(ptx.data(), insertion);
        append(temporalInput.data(), temporalInput.size());
        std::size_t src = insertion;
        const std::size_t half = kExpectedMidpoints / 2;
        for (std::size_t i = 0; i < marks.size(); ++i) {
            append(ptx.data() + src, marks[i] - src);
            append(i < half ? kCurrToPrev : kPrevToCurr, kScaleLength);
            src = marks[i] + midLength;
        }
        append(ptx.data() + src, n - src);
        a_constants = static_cast<std::uint32_t>(marks.size());

        EmitAdaPtxEntry(a_fat, a_fatSize, entry, patched, a_out);
        return VerifyRebuild(a_out, a_fatSize, parameterName, a_why, a_whySize);
    }

    [[nodiscard]] std::size_t CountLineDirective(const std::string& a_text, const char* a_directive) noexcept
    {
        std::size_t count = 0;
        const std::size_t length = std::strlen(a_directive);
        for (std::size_t pos = a_text.find(a_directive); pos != std::string::npos; pos = a_text.find(a_directive, pos + length)) {
            const bool lineStart = pos == 0 || a_text[pos - 1] == '\n';
            const bool lineEnd = pos + length >= a_text.size() || a_text[pos + length] == '\r' || a_text[pos + length] == '\n' ||
                                 a_text[pos + length] == '\0';
            if (lineStart && lineEnd) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] bool HasMidpointMultiply(const std::string& a_ptx) noexcept
    {
        const std::size_t midLength = sizeof(kMidpointBits) - 1;
        const std::size_t mulLength = sizeof(kMulPrefix) - 1;
        for (std::size_t i = 0; i + midLength < a_ptx.size(); ++i) {
            if (std::memcmp(a_ptx.data() + i, kMidpointBits, midLength) != 0 || a_ptx[i + midLength] != ';') {
                continue;
            }
            std::size_t line = i;
            while (line > 0 && a_ptx[line - 1] != '\n') {
                --line;
            }
            if (i - line >= mulLength && std::memcmp(a_ptx.data() + line, kMulPrefix, mulLength) == 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool VerifyBlackwellText(const std::string& a_text, const KernelProfile& a_profile, const char* a_target,
        char* a_why, std::size_t a_whySize)
    {
        const std::string parameterName = std::string(a_profile.entryName) + "_param_0";
        const std::string entrySignature = std::string(".entry ") + a_profile.entryName + "(";
        char paramSignature[160]{};
        std::snprintf(paramSignature, sizeof(paramSignature), " .b8 %s[%u]", parameterName.c_str(), a_profile.paramBytes);
        if (a_text.find(".version ") == std::string::npos) {
            Say(a_why, a_whySize, "the Blackwell program has no PTX version line");
            return false;
        }
        if (a_text.find(entrySignature) == std::string::npos || a_text.find(paramSignature) == std::string::npos) {
            Say(a_why, a_whySize, "the Blackwell program's entry or parameter block is not this kernel's (a different runtime build)");
            return false;
        }
        if (CountLineDirective(a_text, a_target) != 1) {
            std::snprintf(a_why, a_whySize, "the Blackwell program does not carry exactly one '%s' line", a_target);
            return false;
        }
        if (a_profile.role == Role::kMotionVector) {
            if (a_text.find("[" + parameterName + "+32]") == std::string::npos) {
                Say(a_why, a_whySize, "the Blackwell motion-vector program does not read the frame's time from its parameter block");
                return false;
            }
            if (HasMidpointMultiply(a_text)) {
                Say(a_why, a_whySize, "the Blackwell motion-vector program carries a midpoint multiply");
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool VerifyBlackwellRebuild(const std::vector<std::uint8_t>& a_out, std::size_t a_originalSize, const KernelProfile& a_profile,
        char* a_why, std::size_t a_whySize)
    {
        std::size_t entry = 0;
        if (!FindAdaPtxEntry(a_out.data(), a_out.size(), entry)) {
            Say(a_why, a_whySize, "the rebuilt fatbin does not re-parse");
            return false;
        }
        const std::uint32_t hdr = ReadU32(a_out.data() + entry + 4);
        const std::uint64_t payload = ReadU64(a_out.data() + entry + 8);
        if (ReadU32(a_out.data() + entry + 16) != 0 || ReadU64(a_out.data() + entry + 56) != 0 ||
            ReadU64(a_out.data() + entry + 40) != kUncompressedFlags) {
            Say(a_why, a_whySize, "the rebuilt PTX entry is not marked uncompressed");
            return false;
        }
        if (!VerifyContainerEnd(a_out, a_originalSize, entry, hdr, payload, a_why, a_whySize)) {
            return false;
        }
        const auto* const text = reinterpret_cast<const char*>(a_out.data() + entry + hdr);
        const std::string ptx(text, static_cast<std::size_t>(payload));
        if (!VerifyBlackwellText(ptx, a_profile, kTargetAda, a_why, a_whySize)) {
            return false;
        }
        if (CountLineDirective(ptx, kTargetBlackwell) != 0) {
            Say(a_why, a_whySize, "the rebuilt program still targets sm_120");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool BuildBlackwellFatbin(const std::uint8_t* a_fat, std::size_t a_fatSize, const KernelProfile& a_profile,
        std::vector<std::uint8_t>& a_out, std::size_t& a_ptxBytes, char* a_why, std::size_t a_whySize)
    {
        std::size_t adaEntry = 0;
        if (!FindAdaPtxEntry(a_fat, a_fatSize, adaEntry)) {
            Say(a_why, a_whySize, "no sm_89 PTX entry in the kernel's fatbin");
            return false;
        }
        std::size_t entry = 0;
        if (!FindPtxEntry(a_fat, a_fatSize, kBlackwellArch, entry)) {
            Say(a_why, a_whySize, "no sm_120 PTX entry in the kernel's fatbin (this build ships no Blackwell program for it)");
            return false;
        }
        const std::uint32_t hdr = ReadU32(a_fat + entry + 4);
        const std::uint32_t compressed = ReadU32(a_fat + entry + 16);
        const std::uint64_t raw = ReadU64(a_fat + entry + 56);
        if (compressed == 0 || raw == 0 || raw > (8U << 20)) {
            Say(a_why, a_whySize, "the sm_120 PTX entry is not compressed the way this correction expects");
            return false;
        }
        if (raw != a_profile.blackwellPtxBytes) {
            std::snprintf(a_why, a_whySize, "the Blackwell PTX is %llu bytes, expected %zu",
                static_cast<unsigned long long>(raw), a_profile.blackwellPtxBytes);
            return false;
        }
        std::vector<std::uint8_t> ptx(static_cast<std::size_t>(raw));
        if (!Lz4BlockDecompress(a_fat + entry + hdr, compressed, ptx.data(), ptx.size())) {
            Say(a_why, a_whySize, "the Blackwell PTX did not decompress");
            return false;
        }
        a_ptxBytes = ptx.size();
        std::string text(reinterpret_cast<const char*>(ptx.data()), ptx.size());
        if (!VerifyBlackwellText(text, a_profile, kTargetBlackwell, a_why, a_whySize)) {
            return false;
        }
        std::size_t pos = text.find(kTargetBlackwell);
        while (pos != std::string::npos && pos != 0 && text[pos - 1] != '\n') {
            pos = text.find(kTargetBlackwell, pos + 1);
        }
        if (pos == std::string::npos) {
            Say(a_why, a_whySize, "the target line was not found where it was counted");
            return false;
        }
        text.replace(pos, sizeof(kTargetBlackwell) - 1, kTargetAda);
        const std::vector<std::uint8_t> retargeted(text.begin(), text.end());
        EmitAdaPtxEntry(a_fat, a_fatSize, adaEntry, retargeted, a_out);
        return VerifyBlackwellRebuild(a_out, a_fatSize, a_profile, a_why, a_whySize);
    }

    struct Located
    {
        const std::uint8_t* fat{ nullptr };
        std::size_t fatSize{ 0 };
        const KernelProfile* profile{ nullptr };
        std::vector<std::uint64_t*> slots;
    };

    [[nodiscard]] bool LocateRoles(void* a_module, const Role* a_roles, std::size_t a_count, std::vector<Located>& a_out,
        char* a_why, std::size_t a_whySize)
    {
        a_out.clear();
        a_out.resize(a_count);
        if (a_module == nullptr || a_roles == nullptr || a_count == 0) {
            Say(a_why, a_whySize, "no module");
            return false;
        }
        auto* const base = reinterpret_cast<std::uint8_t*>(a_module);
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            Say(a_why, a_whySize, "the module is not a mapped image");
            return false;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            Say(a_why, a_whySize, "the module is not a mapped image");
            return false;
        }
        const std::size_t imageSize = nt->OptionalHeader.SizeOfImage;
        const auto start = reinterpret_cast<std::uintptr_t>(base);

        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0) {
                continue;
            }
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
                continue;
            }
            std::uint8_t* const sec = base + section->VirtualAddress;
            const std::size_t size = section->Misc.VirtualSize;
            for (std::size_t off = 0; off + sizeof(std::uint64_t) <= size; off += sizeof(std::uint64_t)) {
                std::uint64_t value = 0;
                std::memcpy(&value, sec + off, sizeof(value));
                if (imageSize < kOuterHeader || value < start || value > start + imageSize - kOuterHeader) {
                    continue;
                }
                const auto* const candidate = reinterpret_cast<const std::uint8_t*>(value);
                if (ReadU32(candidate) != kFatbinMagic) {
                    continue;
                }
                const std::uint64_t declared = ReadU64(candidate + 8);
                if (declared > static_cast<std::uint64_t>(SIZE_MAX - kOuterHeader)) {
                    continue;
                }
                const std::size_t total = static_cast<std::size_t>(declared) + kOuterHeader;
                if (total < 1024 || total > (16U << 20)) {
                    continue;
                }
                if (total > start + imageSize - value) {
                    continue;
                }

                for (std::size_t r = 0; r < a_count; ++r) {
                    const Role role = a_roles[r];
                    const KernelProfile* nameProfile = nullptr;
                    for (const auto& profile : kKernelProfiles) {
                        if (profile.role != role) {
                            continue;
                        }
                        std::uint64_t entryName = 0;
                        std::uint64_t descriptorName = 0;
                        if (!ReadRelativePointer(base, imageSize, sec + off, profile.entryNameOffset, entryName) ||
                            !ReadRelativePointer(base, imageSize, sec + off, profile.descriptorNameOffset, descriptorName)) {
                            continue;
                        }
                        if (PointsToCString(base, imageSize, entryName, profile.entryName) &&
                            PointsToCString(base, imageSize, descriptorName, profile.descriptorName)) {
                            nameProfile = &profile;
                            break;
                        }
                    }
                    if (nameProfile == nullptr) {
                        continue;
                    }
                    const KernelProfile* const fatProfile = MatchKernelProfile(candidate, total, role);
                    if (fatProfile == nullptr || fatProfile != nameProfile) {
                        continue;
                    }
                    Located& out = a_out[r];
                    if (out.fat == nullptr) {
                        out.fat = candidate;
                        out.fatSize = total;
                        out.profile = fatProfile;
                    } else if (candidate != out.fat) {
                        continue;
                    }
                    out.slots.push_back(reinterpret_cast<std::uint64_t*>(sec + off));
                    break;
                }
            }
        }

        for (std::size_t r = 0; r < a_count; ++r) {
            const Located& out = a_out[r];
            if (out.fat == nullptr || out.slots.empty() || out.profile == nullptr) {
                std::snprintf(a_why, a_whySize, "no supported %s kernel descriptor found in the runtime",
                    Platform::MfgTemporalFix::RoleName(a_roles[r]));
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool Locate(void* a_module, Role a_role, Located& a_out, char* a_why, std::size_t a_whySize)
    {
        const Role roles[1]{ a_role };
        std::vector<Located> found;
        if (!LocateRoles(a_module, roles, 1, found, a_why, a_whySize)) {
            return false;
        }
        a_out = std::move(found.front());
        return true;
    }

    struct Patch
    {
        std::uint64_t* slot{ nullptr };
        std::uint64_t original{ 0 };
        std::uint64_t target{ 0 };
    };

    std::vector<Patch> g_patches;
    std::vector<void*> g_allocations;

    [[nodiscard]] std::size_t RevertPatches() noexcept
    {
        std::size_t stuck = 0;
        for (std::size_t i = 0; i < g_patches.size(); ++i) {
            const Patch patch = g_patches[i];
            DWORD oldProtect = 0;
            if (::VirtualProtect(patch.slot, sizeof(std::uint64_t), PAGE_READWRITE, &oldProtect) == 0) {
                g_patches[stuck++] = patch;
                continue;
            }
            *patch.slot = patch.original;
            DWORD ignored = 0;
            (void)::VirtualProtect(patch.slot, sizeof(std::uint64_t), oldProtect, &ignored);
        }
        g_patches.resize(stuck);
        return stuck;
    }

    void FreeAllocations() noexcept
    {
        for (void* const memory : g_allocations) {
            if (memory != nullptr) {
                (void)::VirtualFree(memory, 0, MEM_RELEASE);
            }
        }
        g_allocations.clear();
    }

    struct Rebuilt
    {
        Located located;
        std::vector<std::uint8_t> bytes;
        std::uint32_t constants{ 0 };
        std::size_t ptxBytes{ 0 };
    };

    [[nodiscard]] std::size_t RolesFor(Program a_program, Role (&a_roles)[kRoleCount]) noexcept
    {
        a_roles[0] = Role::kMotionVector;
        if (a_program == Program::kMidpoint) {
            return 1;
        }
        a_roles[1] = Role::kInpaint;
        a_roles[2] = Role::kInpaintDecision;
        return kRoleCount;
    }

    [[nodiscard]] bool BuildKernel(Program a_program, Rebuilt& a_kernel, char* a_why, std::size_t a_whySize)
    {
        const KernelProfile& profile = *a_kernel.located.profile;
        char why[200]{};
        const bool built = a_program == Program::kBlackwell
            ? BuildBlackwellFatbin(a_kernel.located.fat, a_kernel.located.fatSize, profile, a_kernel.bytes, a_kernel.ptxBytes, why, sizeof(why))
            : BuildTemporalFatbin(a_kernel.located.fat, a_kernel.located.fatSize, profile, a_kernel.bytes, a_kernel.constants,
                  a_kernel.ptxBytes, why, sizeof(why));
        if (!built) {
            std::snprintf(a_why, a_whySize, "%s kernel: %s", Platform::MfgTemporalFix::RoleName(profile.role), why);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool RebuildWithFallback(void* a_module, Program a_requested, Program& a_program,
        std::vector<Rebuilt>& a_out, Platform::MfgTemporalFix::Report& a_report)
    {
        Role roles[kRoleCount]{};
        const std::size_t count = RolesFor(a_requested, roles);
        std::vector<Located> located;
        if (!LocateRoles(a_module, roles, count, located, a_report.detail, sizeof(a_report.detail))) {
            if (a_requested != Program::kBlackwell) {
                return false;
            }
            std::snprintf(a_report.fallbackReason, sizeof(a_report.fallbackReason), "%s", a_report.detail);
            if (!LocateRoles(a_module, roles, 1, located, a_report.detail, sizeof(a_report.detail))) {
                return false;
            }
            a_program = Program::kMidpoint;
        } else {
            a_program = a_requested;
        }
        a_out.clear();
        a_out.reserve(count);
        for (std::size_t i = 0; i < (a_program == Program::kMidpoint ? std::size_t{ 1 } : count); ++i) {
            Rebuilt kernel;
            kernel.located = std::move(located[i]);
            if (!BuildKernel(a_program, kernel, a_report.detail, sizeof(a_report.detail))) {
                if (a_program != Program::kBlackwell) {
                    return false;
                }
                std::snprintf(a_report.fallbackReason, sizeof(a_report.fallbackReason), "%s", a_report.detail);
                a_program = Program::kMidpoint;
                Rebuilt motionVector;
                motionVector.located = i == 0 ? std::move(kernel.located) : std::move(a_out.front().located);
                a_out.clear();
                if (!BuildKernel(a_program, motionVector, a_report.detail, sizeof(a_report.detail))) {
                    return false;
                }
                a_out.push_back(std::move(motionVector));
                return true;
            }
            a_out.push_back(std::move(kernel));
        }
        return true;
    }

    void Summarise(Platform::MfgTemporalFix::Report& a_report, const std::vector<Rebuilt>& a_rebuilt, Program a_program,
        const char* a_verb) noexcept
    {
        a_report.program = a_program;
        a_report.kernels = static_cast<std::uint32_t>(a_rebuilt.size());
        std::uint32_t slots = 0;
        for (std::size_t k = 0; k < a_rebuilt.size() && k < kRoleCount; ++k) {
            const auto count = static_cast<std::uint32_t>(a_rebuilt[k].located.slots.size());
            a_report.kernelSlots[k] = count;
            slots += count;
        }
        a_report.slots = slots;
        const Rebuilt& first = a_rebuilt.front();
        Say(a_report.profile, sizeof(a_report.profile), first.located.profile->descriptorName);
        a_report.ptxBytes = first.ptxBytes;
        a_report.fatbinBytes = first.located.fatSize;
        a_report.rebuiltBytes = first.bytes.size();
        a_report.constants = first.constants;
        if (a_program == Program::kMidpoint) {
            std::snprintf(a_report.detail, sizeof(a_report.detail),
                "%u %s descriptor(s) %s, %zu-byte fatbin -> %zu-byte rebuild, %u midpoint constants rewritten",
                a_report.slots, first.located.profile->descriptorName, a_verb, a_report.fatbinBytes, a_report.rebuiltBytes,
                a_report.constants);
        } else {
            std::snprintf(a_report.detail, sizeof(a_report.detail),
                "%u descriptor(s) %s across %u kernels (%s, %s, %s): the sm_120 programs retargeted to sm_89 (%zu + %zu + %zu bytes "
                "of PTX) for the driver to build; the midpoint rewrite is not applied",
                a_report.slots, a_verb, a_report.kernels, a_rebuilt[0].located.profile->descriptorName,
                a_rebuilt[1].located.profile->descriptorName, a_rebuilt[2].located.profile->descriptorName, a_rebuilt[0].ptxBytes,
                a_rebuilt[1].ptxBytes, a_rebuilt[2].ptxBytes);
        }
    }
}

namespace Platform::MfgTemporalFix
{
    const char* ProgramName(Program a_program) noexcept
    {
        return a_program == Program::kBlackwell ? "blackwell" : "midpoint";
    }

    const char* RoleName(Role a_role) noexcept
    {
        switch (a_role) {
        case Role::kInpaint:
            return "inpaint";
        case Role::kInpaintDecision:
            return "inpaint-decision";
        case Role::kMotionVector:
        default:
            return "motion-vector";
        }
    }

    Report Probe(void* a_module, Program a_program) noexcept
    {
        Report report{};
        try {
            std::vector<Rebuilt> rebuilt;
            Program program = a_program;
            if (!RebuildWithFallback(a_module, a_program, program, rebuilt, report)) {
                return report;
            }
            Summarise(report, rebuilt, program, "found");
            report.ok = true;
            return report;
        } catch (...) {
            Say(report.detail, sizeof(report.detail), "an exception while reading the runtime's kernel");
            report.ok = false;
            return report;
        }
    }

    Report Apply(void* a_module, Program a_program) noexcept
    {
        Report report{};
        try {
            if (!g_patches.empty()) {
                Say(report.detail, sizeof(report.detail), "already applied");
                return report;
            }
            std::vector<Rebuilt> rebuilt;
            Program program = a_program;
            if (!RebuildWithFallback(a_module, a_program, program, rebuilt, report)) {
                return report;
            }

            std::size_t totalSlots = 0;
            for (const auto& kernel : rebuilt) {
                totalSlots += kernel.located.slots.size();
            }
            g_patches.clear();
            g_patches.reserve(totalSlots);
            g_allocations.clear();
            g_allocations.reserve(rebuilt.size());
            for (const auto& kernel : rebuilt) {
                void* const memory = ::VirtualAlloc(nullptr, kernel.bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (memory == nullptr) {
                    FreeAllocations();
                    Say(report.detail, sizeof(report.detail), "the corrected fatbin could not be allocated");
                    return report;
                }
                std::memcpy(memory, kernel.bytes.data(), kernel.bytes.size());
                g_allocations.push_back(memory);
            }
            for (std::size_t k = 0; k < rebuilt.size(); ++k) {
                for (std::uint64_t* const slot : rebuilt[k].located.slots) {
                    g_patches.push_back({ slot, *slot, reinterpret_cast<std::uint64_t>(g_allocations[k]) });
                }
            }

            std::size_t published = 0;
            for (; published < g_patches.size(); ++published) {
                const Patch& patch = g_patches[published];
                DWORD oldProtect = 0;
                if (::VirtualProtect(patch.slot, sizeof(std::uint64_t), PAGE_READWRITE, &oldProtect) == 0) {
                    break;
                }
                *patch.slot = patch.target;
                DWORD ignored = 0;
                (void)::VirtualProtect(patch.slot, sizeof(std::uint64_t), oldProtect, &ignored);
            }

            if (published != g_patches.size()) {
                const std::size_t attempted = g_patches.size();
                g_patches.resize(published);
                const std::size_t stuck = RevertPatches();
                std::snprintf(report.detail, sizeof(report.detail),
                    "only %zu of %zu descriptor slot(s) were writable; the correction was rolled back%s",
                    published, attempted,
                    stuck == 0 ? "" : " INCOMPLETELY - the runtime's own kernel may be partly redirected");
                if (stuck == 0) {
                    FreeAllocations();
                }
                return report;
            }

            Summarise(report, rebuilt, program, "redirected");
            report.ok = true;
            return report;
        } catch (...) {
            const std::size_t stuck = RevertPatches();
            if (stuck == 0) {
                FreeAllocations();
            }
            Say(report.detail, sizeof(report.detail),
                stuck == 0 ? "an exception while applying the correction; nothing was left redirected"
                           : "an exception while applying the correction, and a descriptor could not be reverted");
            report.ok = false;
            return report;
        }
    }

    void Restore() noexcept
    {
        if (RevertPatches() != 0) {
            return;
        }
        FreeAllocations();
    }

    bool ExtractFatbin(void* a_module, Program a_program, Role a_role, std::vector<std::uint8_t>& a_out, const char*& a_entryName,
        char* a_why, std::size_t a_whySize) noexcept
    {
        try {
            if (a_program == Program::kMidpoint && a_role != Role::kMotionVector) {
                Say(a_why, a_whySize, "the midpoint correction has one kernel (the motion-vector estimate)");
                return false;
            }
            Located located;
            if (!Locate(a_module, a_role, located, a_why, a_whySize)) {
                return false;
            }
            const KernelProfile& profile = *located.profile;
            std::uint32_t constants = 0;
            std::size_t ptxBytes = 0;
            const bool built = a_program == Program::kBlackwell
                ? BuildBlackwellFatbin(located.fat, located.fatSize, profile, a_out, ptxBytes, a_why, a_whySize)
                : BuildTemporalFatbin(located.fat, located.fatSize, profile, a_out, constants, ptxBytes, a_why, a_whySize);
            if (!built) {
                return false;
            }
            a_entryName = profile.entryName;
            return true;
        } catch (...) {
            Say(a_why, a_whySize, "an exception while extracting the container");
            return false;
        }
    }

    bool ExtractOriginalFatbin(void* a_module, Role a_role, std::vector<std::uint8_t>& a_out, const char*& a_entryName, char* a_why,
        std::size_t a_whySize) noexcept
    {
        try {
            Located located;
            if (!Locate(a_module, a_role, located, a_why, a_whySize)) {
                return false;
            }
            a_out.assign(located.fat, located.fat + located.fatSize);
            a_entryName = located.profile->entryName;
            return true;
        } catch (...) {
            Say(a_why, a_whySize, "an exception while copying the container");
            return false;
        }
    }

    bool ExtractProgram(void* a_module, Program a_program, Role a_role, std::vector<char>& a_ptxOut, const char*& a_entryName,
        std::uint32_t& a_sharedBytes, char* a_why, std::size_t a_whySize) noexcept
    {
        try {
            if (a_program == Program::kMidpoint && a_role != Role::kMotionVector) {
                Say(a_why, a_whySize, "the midpoint correction has one kernel (the motion-vector estimate)");
                return false;
            }
            Located located;
            if (!Locate(a_module, a_role, located, a_why, a_whySize)) {
                return false;
            }
            const KernelProfile& profile = *located.profile;
            std::vector<std::uint8_t> bytes;
            std::uint32_t constants = 0;
            std::size_t ptxBytes = 0;
            const bool built = a_program == Program::kBlackwell
                ? BuildBlackwellFatbin(located.fat, located.fatSize, profile, bytes, ptxBytes, a_why, a_whySize)
                : BuildTemporalFatbin(located.fat, located.fatSize, profile, bytes, constants, ptxBytes, a_why, a_whySize);
            if (!built) {
                return false;
            }
            std::size_t entry = 0;
            if (!FindAdaPtxEntry(bytes.data(), bytes.size(), entry)) {
                Say(a_why, a_whySize, "the rebuilt fatbin does not re-parse");
                return false;
            }
            const std::uint32_t hdr = ReadU32(bytes.data() + entry + 4);
            std::size_t payload = static_cast<std::size_t>(ReadU64(bytes.data() + entry + 8));
            const auto* const text = reinterpret_cast<const char*>(bytes.data() + entry + hdr);
            while (payload > 0 && text[payload - 1] == '\0') {
                --payload;
            }
            a_ptxOut.assign(text, text + payload);
            a_entryName = profile.entryName;
            a_sharedBytes = profile.sharedBytes;
            return true;
        } catch (...) {
            Say(a_why, a_whySize, "an exception while extracting the program");
            return false;
        }
    }

    bool HashFile(const wchar_t* a_path, char* a_sha256Out, std::size_t a_outSize,
        unsigned long long& a_size) noexcept
    {
        Say(a_sha256Out, a_outSize, "unavailable");
        a_size = 0;
        if (a_outSize < 65) {
            return false;
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (::GetFileAttributesExW(a_path, GetFileExInfoStandard, &data) == 0) {
            return false;
        }
        a_size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            return false;
        }
        bool done = false;
        HANDLE file = ::CreateFileW(a_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE &&
            BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
            constexpr std::size_t kChunk = 1U << 20;
            auto* const chunk = static_cast<unsigned char*>(std::malloc(kChunk));
            DWORD got = 0;
            bool ok = chunk != nullptr;
            while (ok && ::ReadFile(file, chunk, static_cast<DWORD>(kChunk), &got, nullptr) && got > 0) {
                if (!BCRYPT_SUCCESS(::BCryptHashData(hash, chunk, got, 0))) {
                    ok = false;
                }
            }
            std::free(chunk);
            unsigned char digest[32]{};
            if (ok && BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
                static constexpr char kHex[] = "0123456789abcdef";
                std::size_t n = 0;
                for (const unsigned char byte : digest) {
                    if (n + 2 >= a_outSize) {
                        break;
                    }
                    a_sha256Out[n++] = kHex[byte >> 4];
                    a_sha256Out[n++] = kHex[byte & 0xF];
                }
                a_sha256Out[n] = '\0';
                done = true;
            }
        }
        if (hash != nullptr) {
            (void)::BCryptDestroyHash(hash);
        }
        if (file != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(file);
        }
        (void)::BCryptCloseAlgorithmProvider(algorithm, 0);
        return done;
    }
}

namespace
{
    constexpr std::uint8_t kGateOld = 0xB0;
    constexpr std::uint8_t kGateNew = 0x90;

    constexpr std::uint8_t kAdvertise[] = { 0x81, 0xFD, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x8C };
    constexpr std::size_t kAdvertiseImm = 2;
    constexpr std::uint8_t kCapability[] = { 0x3D, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x93, 0xC0, 0x88, 0x47, 0x28 };
    constexpr std::size_t kCapabilityImm = 1;

    struct TextSection
    {
        std::uint8_t* begin{ nullptr };
        std::size_t size{ 0 };
    };

    [[nodiscard]] bool FindTextSection(void* a_module, TextSection& a_out, char* a_why, std::size_t a_whySize) noexcept
    {
        if (a_module == nullptr) {
            Say(a_why, a_whySize, "no module");
            return false;
        }
        auto* const base = reinterpret_cast<std::uint8_t*>(a_module);
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            Say(a_why, a_whySize, "the module is not a mapped image");
            return false;
        }
        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            Say(a_why, a_whySize, "the module is not a mapped image");
            return false;
        }
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if (std::memcmp(section->Name, ".text\0\0\0", IMAGE_SIZEOF_SHORT_NAME) != 0) {
                continue;
            }
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section->Misc.VirtualSize == 0) {
                Say(a_why, a_whySize, "the .text section is not executable code");
                return false;
            }
            a_out.begin = base + section->VirtualAddress;
            a_out.size = section->Misc.VirtualSize;
            return true;
        }
        Say(a_why, a_whySize, "the module has no .text section");
        return false;
    }

    [[nodiscard]] std::size_t CountPattern(const std::uint8_t* a_begin, std::size_t a_size,
        const std::uint8_t* a_pattern, std::size_t a_length, const std::uint8_t** a_first) noexcept
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i + a_length <= a_size; ++i) {
            if (std::memcmp(a_begin + i, a_pattern, a_length) != 0) {
                continue;
            }
            if (count == 0 && a_first != nullptr) {
                *a_first = a_begin + i;
            }
            ++count;
        }
        return count;
    }

    [[nodiscard]] std::size_t CountCompares(const std::uint8_t* a_begin, std::size_t a_size) noexcept
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i + 6 <= a_size; ++i) {
            const std::uint8_t* const p = a_begin + i;
            if (p[0] == 0x3D && p[1] == 0xB0 && p[2] == 0x01 && p[3] == 0x00 && p[4] == 0x00) {
                ++count;
            } else if (p[0] == 0x81 && p[1] >= 0xF8 && p[2] == 0xB0 && p[3] == 0x01 && p[4] == 0x00 && p[5] == 0x00) {
                ++count;
            }
        }
        return count;
    }

    struct GateSites
    {
        std::uint8_t* advertise{ nullptr };
        std::uint8_t* capability{ nullptr };
        std::uint32_t advertiseRva{ 0 };
        std::uint32_t capabilityRva{ 0 };
    };

    [[nodiscard]] bool LocateGates(void* a_module, std::uint32_t a_expectedAdvertiseRva,
        std::uint32_t a_expectedCapabilityRva, GateSites& a_out, char* a_why, std::size_t a_whySize) noexcept
    {
        TextSection text;
        if (!FindTextSection(a_module, text, a_why, a_whySize)) {
            return false;
        }
        const std::uint8_t* advertise = nullptr;
        const std::uint8_t* capability = nullptr;
        const std::size_t advertiseCount = CountPattern(text.begin, text.size, kAdvertise, sizeof(kAdvertise), &advertise);
        const std::size_t capabilityCount = CountPattern(text.begin, text.size, kCapability, sizeof(kCapability), &capability);
        const std::size_t compares = CountCompares(text.begin, text.size);
        if (advertiseCount != 1 || capabilityCount != 1 || compares != 2) {
            std::snprintf(a_why, a_whySize,
                "the architecture gates are not the two known compares (advertise x%zu, capability x%zu, "
                "compares against 0x1b0 in .text x%zu; expected 1, 1, 2)",
                advertiseCount, capabilityCount, compares);
            return false;
        }
        auto* const base = reinterpret_cast<std::uint8_t*>(a_module);
        const auto advertiseRva = static_cast<std::uint32_t>(advertise - base);
        const auto capabilityRva = static_cast<std::uint32_t>(capability - base);
        if ((a_expectedAdvertiseRva != 0 && advertiseRva != a_expectedAdvertiseRva) ||
            (a_expectedCapabilityRva != 0 && capabilityRva != a_expectedCapabilityRva)) {
            std::snprintf(a_why, a_whySize,
                "the gates were found at RVA %#x / %#x but this build pins them at %#x / %#x",
                advertiseRva, capabilityRva, a_expectedAdvertiseRva, a_expectedCapabilityRva);
            return false;
        }
        a_out.advertise = const_cast<std::uint8_t*>(advertise) + kAdvertiseImm;
        a_out.capability = const_cast<std::uint8_t*>(capability) + kCapabilityImm;
        a_out.advertiseRva = advertiseRva;
        a_out.capabilityRva = capabilityRva;
        return true;
    }

    struct GatePatch
    {
        std::uint8_t* site{ nullptr };
        std::uint8_t original{ 0 };
    };
    GatePatch g_gatePatches[2];
    std::size_t g_gatePatched = 0;

    [[nodiscard]] bool WriteCodeByte(std::uint8_t* a_site, std::uint8_t a_value) noexcept
    {
        DWORD oldProtect = 0;
        if (::VirtualProtect(a_site, 1, PAGE_EXECUTE_READWRITE, &oldProtect) == 0) {
            return false;
        }
        *a_site = a_value;
        DWORD ignored = 0;
        (void)::VirtualProtect(a_site, 1, oldProtect, &ignored);
        (void)::FlushInstructionCache(::GetCurrentProcess(), a_site, 1);
        return true;
    }
}

namespace Platform::MfgTemporalFix
{
    GateReport ProbeGates(void* a_module, std::uint32_t a_expectedAdvertiseRva,
        std::uint32_t a_expectedCapabilityRva) noexcept
    {
        GateReport report{};
        GateSites sites;
        if (!LocateGates(a_module, a_expectedAdvertiseRva, a_expectedCapabilityRva, sites, report.detail,
                sizeof(report.detail))) {
            return report;
        }
        report.advertiseRva = sites.advertiseRva;
        report.capabilityRva = sites.capabilityRva;
        report.ok = true;
        std::snprintf(report.detail, sizeof(report.detail),
            "2 compare sites against 0x1b0 in .text (advertise at RVA %#x, capability at RVA %#x)",
            sites.advertiseRva, sites.capabilityRva);
        return report;
    }

    GateReport ApplyGates(void* a_module, std::uint32_t a_expectedAdvertiseRva,
        std::uint32_t a_expectedCapabilityRva) noexcept
    {
        GateReport report{};
        if (g_gatePatched == 2) {
            Say(report.detail, sizeof(report.detail), "already applied");
            return report;
        }
        if (g_gatePatched == 1) {
            Say(report.detail, sizeof(report.detail),
                "a previous attempt left the capability gate rewritten and could not put it back");
            return report;
        }
        GateSites sites;
        if (!LocateGates(a_module, a_expectedAdvertiseRva, a_expectedCapabilityRva, sites, report.detail,
                sizeof(report.detail))) {
            return report;
        }
        if (!WriteCodeByte(sites.capability, kGateNew)) {
            Say(report.detail, sizeof(report.detail), "the capability gate could not be made writable; nothing was written");
            return report;
        }
        if (!WriteCodeByte(sites.advertise, kGateNew)) {
            if (WriteCodeByte(sites.capability, kGateOld)) {
                Say(report.detail, sizeof(report.detail),
                    "the advertising gate could not be made writable; the capability gate was put back (verified)");
            } else {
                g_gatePatches[0] = { sites.capability, kGateOld };
                g_gatePatched = 1;
                Say(report.detail, sizeof(report.detail),
                    "the advertising gate could not be made writable and the capability gate could NOT be put back: "
                    "rolled back INCOMPLETELY (capable, still advertising 2x; harmless, not pristine)");
            }
            return report;
        }
        g_gatePatches[0] = { sites.capability, kGateOld };
        g_gatePatches[1] = { sites.advertise, kGateOld };
        g_gatePatched = 2;
        report.advertiseRva = sites.advertiseRva;
        report.capabilityRva = sites.capabilityRva;
        report.rewritten = 2;
        report.ok = true;
        std::snprintf(report.detail, sizeof(report.detail),
            "2 compare sites rewritten 0x1b0 -> 0x190 (advertise at RVA %#x, capability at RVA %#x)",
            sites.advertiseRva, sites.capabilityRva);
        return report;
    }

    void RestoreGates() noexcept
    {
        std::size_t stuck = 0;
        for (std::size_t i = 0; i < g_gatePatched; ++i) {
            if (!WriteCodeByte(g_gatePatches[i].site, g_gatePatches[i].original)) {
                g_gatePatches[stuck++] = g_gatePatches[i];
            }
        }
        g_gatePatched = stuck;
    }
}
