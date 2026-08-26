#pragma once

#include <filesystem>

namespace Config {

    // One-time migration of the pre-rename config file to the current name.
    //
    // Users of the upstream project had their settings in `opensteamtool.toml`;
    // the Amethyst fork reads `amethysttool.toml`. On first launch after
    // updating, copy the legacy file to the new name so existing settings are
    // not silently lost.
    //
    // Semantics (deliberately conservative):
    //   - Never overwrites an existing `newPath` — migration runs at most once,
    //     and a config the user already edited under the new name always wins.
    //   - Copies rather than moves — the legacy file is left untouched so an
    //     older build (or a rollback) still finds its config (backward compat).
    //   - Touches only the file name; the TOML contents/keys are unchanged.
    //
    // Returns true iff a copy was actually performed (i.e. a legacy config was
    // found and migrated). Returns false when nothing was migrated, including
    // on any filesystem error — the caller then proceeds with normal default
    // handling, so a failed migration is never fatal.
    [[nodiscard]] bool MigrateLegacyConfig(const std::filesystem::path& newPath,
                                           const std::filesystem::path& legacyPath);

} // namespace Config
