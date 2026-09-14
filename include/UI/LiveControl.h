#pragma once

#include <cstdint>
#include <utility>

namespace UI
{
    inline void Help(const char* a_description)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0F);
            ImGui::TextUnformatted(a_description);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

namespace UI
{
    inline void SectionHeader(const char* a_label)
    {
        ImGui::Spacing();
        ImGui::SeparatorText(a_label);
        ImGui::Spacing();
    }
}

namespace UI::Planned
{
    inline void Tag(const char* a_whatItWillDo)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(planned)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("%s", a_whatItWillDo);
        }
    }

    inline void Combo(const char* a_label, const char* a_items, const char* a_whatItWillDo)
    {
        int dummy = 0;
        ImGui::BeginDisabled(true);
        ImGui::Combo(a_label, &dummy, a_items);
        ImGui::EndDisabled();
        Tag(a_whatItWillDo);
    }
}

namespace UI::LiveControl
{
    inline constexpr float kResetButtonWidth = 34.0F;

    namespace Detail
    {
        [[nodiscard]] inline bool Begin(const char* a_id, bool a_atDefault)
        {
            ImGui::PushID(a_id);
            ImGui::BeginDisabled(a_atDefault);
            const bool clicked = ImGui::Button("Rst", ImVec2(kResetButtonWidth, 0.0F));
            ImGui::EndDisabled();
            return clicked;
        }

        inline void End()
        {
            ImGui::PopID();
            ImGui::SameLine();
        }
    }

    [[nodiscard]] inline bool ResetButton(const char* a_id, float a_current, float a_default)
    {
        const bool atDefault = a_current == a_default;
        const bool clicked = Detail::Begin(a_id, atDefault);
        if (!atDefault && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("Reset to the built-in default (%.2f)", static_cast<double>(a_default));
        }
        Detail::End();
        return clicked;
    }

    [[nodiscard]] inline bool ResetButton(
        const char* a_id, std::uint32_t a_current, std::uint32_t a_default)
    {
        const bool atDefault = a_current == a_default;
        const bool clicked = Detail::Begin(a_id, atDefault);
        if (!atDefault && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("Reset to the built-in default");
        }
        Detail::End();
        return clicked;
    }

    [[nodiscard]] inline bool ResetButton(const char* a_id, bool a_current, bool a_default)
    {
        const bool atDefault = a_current == a_default;
        const bool clicked = Detail::Begin(a_id, atDefault);
        if (!atDefault && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("Reset to the built-in default (%s)", a_default ? "on" : "off");
        }
        Detail::End();
        return clicked;
    }

    template <class Apply>
    bool Checkbox(const char* a_label, bool& a_value, Apply&& a_apply)
    {
        if (ImGui::Checkbox(a_label, &a_value)) {
            std::forward<Apply>(a_apply)(a_value);
            return true;
        }
        return false;
    }

    template <class Apply>
    bool SliderFloat(
        const char* a_label, float& a_value, float a_min, float a_max, Apply&& a_apply,
        const char* a_format = "%.2f")
    {
        if (ImGui::SliderFloat(a_label, &a_value, a_min, a_max, a_format)) {
            std::forward<Apply>(a_apply)(a_value);
            return true;
        }
        return false;
    }

    template <class Apply>
    bool SliderInt(const char* a_label, int& a_value, int a_min, int a_max, Apply&& a_apply)
    {
        if (ImGui::SliderInt(a_label, &a_value, a_min, a_max)) {
            std::forward<Apply>(a_apply)(a_value);
            return true;
        }
        return false;
    }

    template <class Apply>
    bool Combo(const char* a_label, std::uint32_t& a_value, const char* a_items, Apply&& a_apply)
    {
        int index = static_cast<int>(a_value);
        if (ImGui::Combo(a_label, &index, a_items)) {
            a_value = static_cast<std::uint32_t>(index < 0 ? 0 : index);
            std::forward<Apply>(a_apply)(a_value);
            return true;
        }
        return false;
    }

    template <class Apply>
    bool CheckboxD(const char* a_label, bool& a_value, bool a_default, Apply&& a_apply)
    {
        bool changed = false;
        if (ResetButton(a_label, a_value, a_default)) {
            a_value = a_default;
            a_apply(a_value);
            changed = true;
        }
        if (ImGui::Checkbox(a_label, &a_value)) {
            a_apply(a_value);
            changed = true;
        }
        return changed;
    }

    template <class Apply>
    bool SliderFloatD(
        const char* a_label, float& a_value, float a_min, float a_max, float a_default,
        Apply&& a_apply, const char* a_format = "%.2f")
    {
        bool changed = false;
        if (ResetButton(a_label, a_value, a_default)) {
            a_value = a_default;
            a_apply(a_value);
            changed = true;
        }
        if (ImGui::SliderFloat(a_label, &a_value, a_min, a_max, a_format)) {
            a_apply(a_value);
            changed = true;
        }
        return changed;
    }

    template <class Apply>
    bool SliderIntCommitD(const char* a_label, std::uint32_t& a_value, int a_min, int a_max, std::uint32_t a_default,
        const char* a_format, std::uint32_t& a_shown, Apply&& a_apply)
    {
        bool committed = false;
        if (ResetButton(a_label, a_value, a_default)) {
            a_value = a_default;
            a_apply(a_value);
            committed = true;
        }
        ImGuiStorage* const storage = ImGui::GetStateStorage();
        const ImGuiID key = ImGui::GetID(a_label);
        const bool inFlight = storage != nullptr && storage->GetInt(key, -1) >= 0;
        int shown = inFlight ? storage->GetInt(key, static_cast<int>(a_value)) : static_cast<int>(a_value);
        const bool edited = ImGui::SliderInt(a_label, &shown, a_min, a_max, a_format);
        shown = shown < a_min ? a_min : shown > a_max ? a_max : shown;
        if (edited && storage != nullptr) {
            storage->SetInt(key, shown);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            a_value = static_cast<std::uint32_t>(shown);
            if (storage != nullptr) storage->SetInt(key, -1);
            a_apply(a_value);
            committed = true;
        } else if (!ImGui::IsItemActive() && inFlight && storage != nullptr) {
            storage->SetInt(key, -1);
            shown = static_cast<int>(a_value);
        }
        a_shown = static_cast<std::uint32_t>(shown);
        return committed;
    }

    template <class Apply>
    bool ComboD(
        const char* a_label, std::uint32_t& a_value, std::uint32_t a_default, const char* a_items,
        Apply&& a_apply)
    {
        bool changed = false;
        if (ResetButton(a_label, a_value, a_default)) {
            a_value = a_default;
            a_apply(a_value);
            changed = true;
        }
        int index = static_cast<int>(a_value);
        if (ImGui::Combo(a_label, &index, a_items)) {
            a_value = static_cast<std::uint32_t>(index < 0 ? 0 : index);
            a_apply(a_value);
            changed = true;
        }
        return changed;
    }
}
