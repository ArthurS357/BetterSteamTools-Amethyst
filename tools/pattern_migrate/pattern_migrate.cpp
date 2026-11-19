// pattern_migrate -- find, in a newer steamclient64.dll / steamui.dll, the functions that
// the shadow recorder fingerprinted in an older build, and write a candidate pattern TOML.
// Offline: reads three files, writes one. No hook, no network, no Steam process, not part
// of the DLL. All the logic lives in Utils/SteamMetadata/PatternMigrateCliLogic.h (and the
// engine in PatternMigrateLogic.h), unit-tested.

#include "Utils/Config/CompatAliasLogic.h"
#include "Utils/SteamMetadata/PatternMigrateCliLogic.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace PatternMigrateCliLogic;

namespace {

    constexpr int kExitOk = 0;
    constexpr int kExitUsage = 1;
    constexpr int kExitNotMigrated = 2;   // a required function not migrated, or nothing migrated
    constexpr int kExitError = 3;         // a file could not be read, parsed or written

    void PrintUsage(std::ostream& out) {
        out <<
            "pattern_migrate - find the recorded functions in a newer Steam DLL\n"
            "\n"
            "Usage: pattern_migrate --fingerprint <file.toml> --dll <steamclient64.dll|steamui.dll>\n"
            "                       --out <candidate.toml> [--required <names|old-pattern.toml>] [--force]\n"
            "\n"
            "  --fingerprint <f>  A file the shadow recorder wrote ([shadow] record = true):\n"
            "                     <Steam>\\amethysttool\\fingerprint\\<component>\\<sha256>.toml\n"
            "  --dll <f>          The NEW DLL, read from disk and never loaded.\n"
            "  --out <f>          Where the candidate pattern TOML goes. Refused inside a Steam folder\n"
            "                     (one with steam.exe), over an input (--fingerprint, --dll, --required),\n"
            "                     or over an existing file.\n"
            "  --required <x>     Functions that must be migrated: a comma-separated list\n"
            "                     (A,B,C) or an old pattern TOML whose [Name] tables are the list.\n"
            "  --force            Replace --out if it exists.\n"
            "\n"
            "Prints one line per function: migrated, ambiguous, not found, size mismatch, calls mismatch\n"
            "or needs manual.\n"
            "Exit codes: 0 every required function migrated (and at least one function did);\n"
            "            1 bad arguments; 2 a required function did not migrate, or none did;\n"
            "            3 a file could not be read, parsed or written.\n"
            "With exit 2 and at least one function migrated the candidate is still written, so the\n"
            "rest can be reviewed (use --force to run again over it).\n"
            "Name the result <sha256 of the new DLL>.toml, REVIEW it, try it locally in\n"
            "<Steam>\\amethysttool\\pattern\\<component>\\ and only then publish it.\n";
    }

    std::string ProcessTag() {
#ifdef _WIN32
        return std::to_string(_getpid());
#else
        return std::to_string(getpid());
#endif
    }

    // Reads a whole file, refusing one above `maxBytes`.
    bool ReadFile(const fs::path& p, std::size_t maxBytes, std::vector<std::uint8_t>& out, std::string& error) {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(p, ec);
        if (ec) { error = "cannot read " + Narrow(p) + ": " + ec.message(); return false; }
        if (size == 0 || size > maxBytes) { error = Narrow(p) + ": size outside the supported range"; return false; }
        std::ifstream in(p, std::ios::binary);
        out.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!in || in.gcount() != static_cast<std::streamsize>(out.size())) {
            error = "cannot read all of " + Narrow(p);
            return false;
        }
        return true;
    }

    std::string_view AsText(const std::vector<std::uint8_t>& b) {
        return { reinterpret_cast<const char*>(b.data()), b.size() };
    }

    // --required: an existing file is an old pattern TOML, anything else is a name list.
    std::optional<std::vector<std::string>> ResolveRequired(const fs::path& value, std::string& error) {
        if (value.empty()) return std::vector<std::string>{};
        std::error_code ec;
        std::optional<std::vector<std::string>> names;
        if (fs::is_regular_file(value, ec)) {
            std::vector<std::uint8_t> bytes;
            if (!ReadFile(value, kMaxFingerprintFileBytes, bytes, error)) return std::nullopt;
            names = ParseRequiredFromPatternToml(AsText(bytes));
            if (!names) error = Narrow(value) + ": no [Name] table, or a name with characters other than A-Z a-z 0-9 _";
        } else {
            names = ParseRequiredList(Narrow(value));
            if (!names) error = "--required: not a file, and not a comma-separated list of function names";
        }
        return names;
    }

    int Migrate(const Options& o) {
        std::string error;
        const auto fail = [&]() { std::cerr << "error: " << error << "\n"; return kExitError; };

        std::vector<std::uint8_t> fpBytes;
        if (!ReadFile(o.fingerprint, kMaxFingerprintFileBytes, fpBytes, error)) return fail();
        const auto fp = ParseFingerprintFile(AsText(fpBytes), error);
        if (!fp) { error = Narrow(o.fingerprint) + ": " + error; return fail(); }

        const auto required = ResolveRequired(o.required, error);
        if (!required) return fail();
        for (const std::string& name : *required) {
            const bool known = std::any_of(fp->fps.begin(), fp->fps.end(),
                                           [&](const auto& f) { return f.name == name; });
            if (!known) std::cerr << "warning: required function " << name << " is not in the fingerprint file\n";
        }

        // Refuse the output before the scan: a bad --out should not cost a run. --required
        // counts as an input only when it is a file.
        std::vector<fs::path> inputs = { o.dll, o.fingerprint };
        std::error_code ec;
        if (!o.required.empty() && fs::is_regular_file(o.required, ec)) inputs.push_back(o.required);
        error = RefuseOutput(o.out, inputs, o.force);
        if (!error.empty()) return fail();

        std::vector<std::uint8_t> dll;
        if (!ReadFile(o.dll, PatternRecorderLogic::kMaxFileBytes, dll, error)) return fail();
        const auto img = MakeImageView(dll, error);
        if (!img) { error = Narrow(o.dll) + ": " + error; return fail(); }

        const RunResult run = RunMigration(*img, *fp, *required);
        std::cout << "fingerprint: " << fp->component << " " << fp->sha256 << "\n" << RenderReport(*fp, run);

        if (run.migrated == 0) {
            std::cerr << "error: nothing migrated; no file written\n";
            return kExitNotMigrated;
        }
        const std::string body = RenderCandidateToml(*fp, run);
        const std::error_code werr = CompatAliasLogic::detail::WriteBytesViaTemp(
            body, o.out, CompatAliasLogic::detail::MakeTempTag(ProcessTag()));
        if (werr) { error = "cannot write " + Narrow(o.out) + ": " + werr.message(); return fail(); }
        std::cout << "wrote " << Narrow(o.out) << "\n";
        std::cout.flush();
        if (!std::cout) { std::cerr << "error: cannot write to standard output\n"; return kExitError; }
        return ExitCodeFor(run) == 0 ? kExitOk : kExitNotMigrated;
    }

    int Main(int argc, fs::path::value_type** argv) {
        std::vector<fs::path> args;
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

        const ParsedArgs parsed = ParseArgs(args);
        if (!parsed.error.empty()) {
            std::cerr << "error: " << parsed.error << "\n\n";
            PrintUsage(std::cerr);
            return kExitUsage;
        }
        if (parsed.options.help) {
            PrintUsage(std::cout);
            return kExitOk;
        }
        return Migrate(parsed.options);
    }

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        return Main(argc, argv);
    } catch (const std::exception& e) {   // e.g. std::bad_alloc on a hostile file
        std::cerr << "error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "error: unexpected failure\n";
    }
    return kExitError;
}
