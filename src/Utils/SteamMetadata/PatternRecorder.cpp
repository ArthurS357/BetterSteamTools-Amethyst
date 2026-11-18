#include "Utils/SteamMetadata/PatternRecorder.h"

#include "OSTPlatform/include/Hash.h"
#include "Utils/SteamMetadata/PatternRecorderLogic.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <span>
#include <system_error>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace PatternRecorder {

std::optional<std::string> BuildFingerprintFile(const std::filesystem::path& dll, const std::string& component,
                                                const std::string& sha256,
                                                const std::vector<PatternMigrateLogic::NamedRva>& resolved,
                                                std::string* error)
{
    using namespace PatternRecorderLogic;

    const auto fail = [error](const char* why) -> std::optional<std::string> {
        if (error) *error = why;
        return std::nullopt;
    };

    if (!IsSafeComponent(component) || !IsSha256Hex(sha256))
        return fail("component or sha256 is not a safe path segment");

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(dll, ec);
    if (ec) return fail("cannot stat the DLL");
    if (size == 0 || size > kMaxFileBytes) return fail("the DLL size is outside the supported range");

    // The file is read ONCE, into one buffer, and everything below slices that buffer: the
    // bytes that are hashed, parsed and fingerprinted are the same bytes.
    std::ifstream in(dll, std::ios::binary);
    if (!in) return fail("cannot open the DLL");
    std::vector<std::uint8_t> all(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(all.data()), static_cast<std::streamsize>(all.size()));
    if (in.gcount() != static_cast<std::streamsize>(all.size())) return fail("cannot read the whole DLL");

    // The output is named after the SHA-256 the pattern loader computed seconds earlier. If
    // Steam replaced the DLL since, these bytes belong to another build and recording them
    // under the old name would put a wrong fingerprint where "already recorded" keeps it.
    if (OSTPlatform::Hash::Sha256OfBuffer(all.data(), all.size()) != sha256)
        return fail("the DLL no longer matches its SHA-256 (replaced since it was hashed)");

    const std::span<const std::uint8_t> file(all);
    const std::optional<PeLayout> layout = ParsePeLayout(file.first(std::min(file.size(), kHeaderWindow)), file.size());
    if (!layout) return fail("not a PE32+ x64 image with a usable .text and exception directory");

    // ParsePeLayout checked both ranges against the file size.
    PatternMigrateLogic::ImageView img;
    img.textRva = layout->text.rva;
    img.text = file.subspan(static_cast<std::size_t>(layout->text.fileOffset), layout->text.size);
    img.pdata = PatternMigrateLogic::ParsePdata(
        file.subspan(static_cast<std::size_t>(layout->pdata.fileOffset), layout->pdata.size));

    const RecordedSet set = RecordFunctions(img, resolved);
    if (set.fps.empty()) return fail("no function could be fingerprinted");
    return SerializeFingerprints(component, sha256, set);
}

void UseBackgroundPriority() noexcept
{
#if defined(_WIN32)
    // THREAD_MODE_BACKGROUND_BEGIN lowers CPU, I/O and memory priority of this thread only.
    (void)::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
}

} // namespace PatternRecorder
