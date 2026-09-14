#pragma once

namespace UI::Osd
{
    [[nodiscard]] bool Visible() noexcept;

    void Toggle();

    void ServicePendingFontLoad();

    void RequestFontReload() noexcept;

    void NotifyContextReset() noexcept;

    void Draw(bool a_menuOpen);

    [[nodiscard]] const char* FontStatus() noexcept;
}
