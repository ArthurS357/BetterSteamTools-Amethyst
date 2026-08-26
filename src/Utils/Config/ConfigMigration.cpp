#include "Utils/Config/ConfigMigration.h"

#include <system_error>

namespace Config {

    bool MigrateLegacyConfig(const std::filesystem::path& newPath,
                             const std::filesystem::path& legacyPath) {
        namespace fs = std::filesystem;
        std::error_code ec;

        // Target already present: never overwrite. Migration is one-time and a
        // config under the new name always takes precedence.
        if (fs::exists(newPath, ec))
            return false;

        // Nothing to migrate.
        if (!fs::exists(legacyPath, ec))
            return false;

        // copy_options::none => fail (via ec) rather than overwrite; combined
        // with the exists() check above this is redundant but explicit.
        fs::copy_file(legacyPath, newPath, fs::copy_options::none, ec);
        return !ec;
    }

} // namespace Config
