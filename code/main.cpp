#include <raylib.h>
#include <archive.h>
#include <archive_entry.h>
#include <magic.h>
#include <vips/vips.h>
#include <openssl/evp.h>
#include <zlib.h>
#include "rayicons.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cctype>
#include <climits>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <unordered_map>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;


enum class EntryKind { Directory, File, Archive };
enum class ViewMode { Details = 1, List = 2, MediumIcons = 3, LargeIcons = 4 };
enum class SortKey { Name, Size, Date, Type };
enum class TextField { None, Search, Address, Rename, NewItem, Mode, ArchiveOutput, ConvertOutput, ExtractDestination, ArchivePassword, Command, ThemeNumber, EditorConfig, TermConfig, AccentConfig, FontScale };
enum class ArchiveFormat { Tar, Pax, Ustar, Zip, SevenZip, Cpio };
enum class ArchiveCompression { None, Gzip, Bzip2, Xz, Zstd, Lz4, Lzma, Lzip };

enum class Modal { None, Rename, NewItem, Properties, DiskInfo, CreateArchive, ExtractArchive, ConvertImage, Cat, ImageView, Command, Help, About, ThemePicker, PasteOverwrite, ConfirmPermanentDelete, Checksum };
enum class MenuAction { None, Open, Cat, ViewImage, OpenTerminal, ConvertImage, Rename, Copy, CopyPath, Cut, Paste, Delete, PermanentDelete, Compress, Extract, OpenEditor, Refresh, NewDirectory, NewFile, Checksum, Properties };

struct VfsEntry {
    std::string name;
    std::string path;
    EntryKind kind{EntryKind::File};
    uint64_t size{0};
    std::time_t mtime{0};
};

struct VfsProvider {
    virtual ~VfsProvider() = default;
    virtual std::string displayPath() const = 0;
    virtual std::vector<VfsEntry> list(const std::string& path) = 0;
    virtual bool isDirectory(const std::string& path) = 0;
    virtual std::optional<std::string> parent(const std::string& path) = 0;
    virtual void setShowHidden(bool) {}
    virtual bool isLocal() const { return false; }
};

static bool isCtrlDown() { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
static bool isShiftDown() { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }
static bool isAltDown() { return IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT); }

// Fast, responsive key-repeat for navigation. Initial press is immediate,
// then there is a short typematic delay followed by a fast repeat interval.
static bool keyRepeatPressed(int key, double initialDelay = 0.18, double repeatInterval = 0.028) {
    static std::array<double, 512> nextRepeat{};
    static std::array<unsigned char, 512> active{};
    if (key < 0 || key >= (int)nextRepeat.size()) return IsKeyPressed(key);
    const double now = GetTime();
    if (IsKeyPressed(key)) {
        active[(size_t)key] = 1;
        nextRepeat[(size_t)key] = now + initialDelay;
        return true;
    }
    if (IsKeyDown(key)) {
        if (!active[(size_t)key]) {
            active[(size_t)key] = 1;
            nextRepeat[(size_t)key] = now + initialDelay;
            return true;
        }
        if (now >= nextRepeat[(size_t)key]) {
            nextRepeat[(size_t)key] = now + repeatInterval;
            return true;
        }
        return false;
    }
    active[(size_t)key] = 0;
    return false;
}

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

static bool wildcardMatchCI(const std::string& text, const std::string& pattern) {
    const std::string s = lowerCopy(text), p = lowerCopy(pattern);
    size_t si = 0, pi = 0, star = std::string::npos, match = 0;
    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == s[si])) { ++si; ++pi; continue; }
        if (pi < p.size() && p[pi] == '*') { star = pi++; match = si; continue; }
        if (star != std::string::npos) { pi = star + 1; si = ++match; continue; }
        return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

static bool fuzzyMatch(const std::string& haystack, const std::string& query) {
    if (query.empty()) return true;
    if (query.find('*') != std::string::npos || query.find('?') != std::string::npos) return wildcardMatchCI(haystack, query);
    return lowerCopy(haystack).find(lowerCopy(query)) != std::string::npos;
}

static bool isHiddenName(const std::string& name) { return !name.empty() && name[0] == '.' && name != "." && name != ".."; }

static std::time_t fileTimeToTimeT(fs::file_time_type ft) {
    using namespace std::chrono;
    const auto sctp = time_point_cast<system_clock::duration>(
        ft - fs::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sctp);
}

static bool hasArchiveExt(const std::string& name) {
    const std::string n = lowerCopy(name);
    static const char* exts[] = {
        ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz",
        ".tar.zst", ".tzst", ".tar.lz4", ".tlz", ".zip", ".7z", ".rar", ".rar5",
        ".cpio", ".cab", ".iso", ".ar", ".xar", ".lha", ".lzh", ".gz", ".bz2",
        ".xz", ".zst", ".lz4", ".lz", ".lzip"
    };
    for (const char* ext : exts) {
        const size_t len = std::strlen(ext);
        if (n.size() >= len && n.compare(n.size() - len, len, ext) == 0) return true;
    }
    return false;
}

class LocalVfs final : public VfsProvider {
public:
    explicit LocalVfs(fs::path root, bool showHidden=false) : root_(std::move(root)), showHidden_(showHidden) {}
    void setShowHidden(bool v) override { showHidden_ = v; }
    std::string displayPath() const override { return root_.string(); }
    bool isLocal() const override { return true; }

    std::vector<VfsEntry> list(const std::string& path) override {
        std::vector<VfsEntry> out;
        std::error_code ec;
        const fs::path p(path);
        if (!fs::is_directory(p, ec)) return out;
        for (const auto& de : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code xec;
            VfsEntry e;
            e.name = de.path().filename().string();
            if (!showHidden_ && isHiddenName(e.name)) continue;
            e.path = de.path().string();
            const bool dir = de.is_directory(xec);
            if (!xec && dir) {
                e.kind = EntryKind::Directory;
            } else {
                xec.clear();
                const bool regular = de.is_regular_file(xec);
                e.kind = (!xec && regular && hasArchiveExt(e.name)) ? EntryKind::Archive : EntryKind::File;
                xec.clear();
                if (!xec && regular) e.size = (uint64_t)de.file_size(xec);
            }
            xec.clear();
            const auto ft = de.last_write_time(xec);
            if (!xec) e.mtime = fileTimeToTimeT(ft);
            out.push_back(std::move(e));
        }
        std::sort(out.begin(), out.end(), [](const VfsEntry& a, const VfsEntry& b) {
            if ((a.kind == EntryKind::Directory) != (b.kind == EntryKind::Directory)) return a.kind == EntryKind::Directory;
            return lowerCopy(a.name) < lowerCopy(b.name);
        });
        return out;
    }

    bool isDirectory(const std::string& path) override {
        std::error_code ec;
        return fs::is_directory(path, ec);
    }

    std::optional<std::string> parent(const std::string& path) override {
        const fs::path p(path);
        const fs::path par = p.parent_path();
        if (par == p || par.empty()) return std::nullopt;
        return par.string();
    }
private:
    fs::path root_;
    bool showHidden_ = false;
};

class ArchiveVfs final : public VfsProvider {
public:
    explicit ArchiveVfs(std::string archivePath, bool showHidden=false) : showHidden_(showHidden), archivePath_(std::move(archivePath)) { load(); }
    void setShowHidden(bool v) override { showHidden_ = v; }
    bool valid() const { return ok_; }
    const std::string& error() const { return error_; }
    std::string displayPath() const override { return archivePath_ + " :: " + current_; }

    std::vector<VfsEntry> list(const std::string& path) override {
        std::vector<VfsEntry> out;
        std::string prefix = path;
        if (prefix.empty() || prefix == "/") prefix.clear();
        while (!prefix.empty() && prefix.front() == '/') prefix.erase(prefix.begin());
        if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
        for (const auto& e : entries_) {
            if (!prefix.empty()) {
                if (e.path.rfind("/" + prefix, 0) != 0) continue;
            } else if (e.path.rfind("/", 0) != 0) continue;
            const std::string rem = e.path.substr(prefix.empty() ? 1 : prefix.size() + 1);
            if (rem.empty()) continue;
            const size_t slash = rem.find('/');
            if (slash == std::string::npos) {
                VfsEntry x = e;
                x.name = rem;
                if (!showHidden_ && isHiddenName(x.name)) continue;
                out.push_back(std::move(x));
            } else if (slash > 0) {
                const std::string child = rem.substr(0, slash);
                if (!showHidden_ && isHiddenName(child)) continue;
                const std::string childPath = "/" + prefix + child;
                bool seen = false;
                for (const auto& x : out) if (x.path == childPath) { seen = true; break; }
                if (!seen) out.push_back({child, childPath, EntryKind::Directory, 0, 0});
            }
        }
        std::sort(out.begin(), out.end(), [](const VfsEntry& a, const VfsEntry& b) {
            if ((a.kind == EntryKind::Directory) != (b.kind == EntryKind::Directory)) return a.kind == EntryKind::Directory;
            return lowerCopy(a.name) < lowerCopy(b.name);
        });
        return out;
    }

    bool isDirectory(const std::string& path) override {
        if (path.empty() || path == "/") return true;
        return std::any_of(entries_.begin(), entries_.end(), [&](const VfsEntry& e){ return e.kind == EntryKind::Directory && e.path == path; });
    }

    std::optional<std::string> parent(const std::string& path) override {
        if (path.empty() || path == "/") return std::nullopt;
        fs::path p(path);
        std::string par = p.parent_path().string();
        if (par.empty()) par = "/";
        return par;
    }

private:
    bool showHidden_ = false;
    void load() {
        archive* a = archive_read_new();
        if (!a) { error_ = "archive_read_new failed"; return; }
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        if (archive_read_open_filename(a, archivePath_.c_str(), 10240) != ARCHIVE_OK) {
            error_ = archive_error_string(a) ? archive_error_string(a) : "Cannot open archive";
            archive_read_free(a);
            return;
        }
        archive_entry* ae = nullptr;
        while (archive_read_next_header(a, &ae) == ARCHIVE_OK) {
            const char* n = archive_entry_pathname(ae);
            if (!n) { archive_read_data_skip(a); continue; }
            std::string p = n;
            while (!p.empty() && p.front() == '/') p.erase(p.begin());
            while (!p.empty() && p.back() == '/') p.pop_back();
            if (p.empty()) { archive_read_data_skip(a); continue; }
            VfsEntry e;
            e.name = fs::path(p).filename().string();
            e.path = "/" + p;
            e.kind = archive_entry_filetype(ae) == AE_IFDIR ? EntryKind::Directory : EntryKind::File;
            e.size = (uint64_t)std::max<int64_t>(0, archive_entry_size(ae));
            e.mtime = archive_entry_mtime(ae);
            entries_.push_back(std::move(e));
            archive_read_data_skip(a);
        }
        std::vector<VfsEntry> dirs;
        for (const auto& e : entries_) {
            fs::path p = fs::path(e.path).parent_path();
            while (!p.empty() && p != "/") {
                const std::string ps = p.string();
                const bool exists = std::any_of(entries_.begin(), entries_.end(), [&](const VfsEntry& x){ return x.kind == EntryKind::Directory && x.path == ps; });
                const bool queued = std::any_of(dirs.begin(), dirs.end(), [&](const VfsEntry& x){ return x.path == ps; });
                if (!exists && !queued) dirs.push_back({p.filename().string(), ps, EntryKind::Directory, 0, 0});
                p = p.parent_path();
            }
        }
        entries_.insert(entries_.end(), dirs.begin(), dirs.end());
        archive_read_free(a);
        ok_ = true;
    }
    std::string archivePath_;
    std::string current_{"/"};
    std::string error_;
    bool ok_{false};
    std::vector<VfsEntry> entries_;
};

static std::string formatBytes(uint64_t b) {
    const char* units[] = {"bytes", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[64];
    if (u == 0) std::snprintf(buf, sizeof(buf), "%llu %s", (unsigned long long)b, units[u]);
    else std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}

static std::string formatTime(std::time_t t) {
    if (!t) return "";
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

static Color colorHex(unsigned int rgb, unsigned char a = 255) {
    return Color{(unsigned char)((rgb >> 16) & 255), (unsigned char)((rgb >> 8) & 255), (unsigned char)(rgb & 255), a};
}

static float gUIFontScale = 1.15f;
static inline int uiFont(int base) { return std::max(1, (int)std::lround(base * gUIFontScale)); }

struct Theme {
    const char* name;
    Color bg;
    Color panel;
    Color panel2;
    Color text;
    Color muted;
    Color line;
    Color hover;
    Color accent;
};

static const std::array<Theme, 11> kThemes = {{
    {"OLED Black & White", colorHex(0x000000), colorHex(0x000000), colorHex(0x000000), colorHex(0xffffff), colorHex(0xc8c8c8), colorHex(0x303030), colorHex(0x111111), colorHex(0xffffff)},
    {"Midnight",  colorHex(0x050912), colorHex(0x0d1420), colorHex(0x151f2e), colorHex(0xf4f7fb), colorHex(0x8ca1bc), colorHex(0x263448), colorHex(0x122236), colorHex(0x5ca9ff)},
    {"Carbon",    colorHex(0x0a0a0a), colorHex(0x111111), colorHex(0x1b1b1b), colorHex(0xf2f2f2), colorHex(0x969696), colorHex(0x303030), colorHex(0x1c1c1c), colorHex(0xffa640)},
    {"Ocean",     colorHex(0x020b12), colorHex(0x07151e), colorHex(0x0d222d), colorHex(0xeefbff), colorHex(0x8aa6b3), colorHex(0x23404f), colorHex(0x0e2836), colorHex(0x36c5ff)},
    {"Forest",    colorHex(0x030a06), colorHex(0x08140d), colorHex(0x0f2016), colorHex(0xeefff2), colorHex(0x8da894), colorHex(0x274331), colorHex(0x10301e), colorHex(0x42e37b)},
    {"Wine",      colorHex(0x0d0308), colorHex(0x1a080f), colorHex(0x28101b), colorHex(0xfff1f6), colorHex(0xb18d9d), colorHex(0x4b2737), colorHex(0x321321), colorHex(0xff4f8a)},
    {"Purple",    colorHex(0x08030d), colorHex(0x12091b), colorHex(0x1c102a), colorHex(0xf8f0ff), colorHex(0xa894b7), colorHex(0x3b2850), colorHex(0x25153b), colorHex(0xb678ff)},
    {"Amber",     colorHex(0x0b0802), colorHex(0x171107), colorHex(0x241a0b), colorHex(0xfff8e9), colorHex(0xb0a17f), colorHex(0x4a3b20), colorHex(0x2c210e), colorHex(0xffc857)},
    {"Steel",     colorHex(0x06080a), colorHex(0x101418), colorHex(0x181e23), colorHex(0xf0f4f7), colorHex(0x8e9aa2), colorHex(0x2a343c), colorHex(0x1a242a), colorHex(0x7bd7ff)},
    {"Lime",      colorHex(0x050805), colorHex(0x0b120b), colorHex(0x152015), colorHex(0xf3fff0), colorHex(0x93aa8e), colorHex(0x2b4328), colorHex(0x122612), colorHex(0xb5ff53)},
    {"Paper White",colorHex(0xffffff), colorHex(0xffffff), colorHex(0xf3f3f3), colorHex(0x101010), colorHex(0x555555), colorHex(0xc8c8c8), colorHex(0xe8e8e8), colorHex(0x0078d4)}
}};

struct Config {
    int theme = 0;
    Color accent = kThemes[0].accent;
    bool accentCustom = false;
    bool showHidden = false;
    bool showThumbnails = true;
    int thumbnailRes = 3; // 1=32, 2=64, 3=128, 4=256, 5=512
    float fontScale = 1.15f;
    std::string editor;
    std::string term = "foot";
    std::vector<std::string> favorites;
    fs::path dir;
    fs::path file;
};

static fs::path configBase() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg);
    const char* home = std::getenv("HOME");
    if (home && *home) return fs::path(home) / ".config";
    return fs::path(".");
}

static fs::path homeDir() {
    const char* h = std::getenv("HOME");
    return (h && *h) ? fs::path(h) : fs::current_path();
}

static fs::path xdgDataHome() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return fs::path(xdg);
    return homeDir() / ".local" / "share";
}

static fs::path xdgTrashHome() {
    return xdgDataHome() / "Trash";
}

static std::optional<fs::path> xdgUserDir(const char* key, const char* fallback) {
    const fs::path cfg = homeDir() / ".config/user-dirs.dirs";
    std::ifstream in(cfg);
    std::string line;
    const std::string needle = std::string("XDG_") + key + "_DIR=";
    while (std::getline(in, line)) {
        if (line.rfind(needle, 0) != 0) continue;
        std::string value = line.substr(needle.size());
        if (!value.empty() && value.front() == '"') value.erase(value.begin());
        if (!value.empty() && value.back() == '"') value.pop_back();
        const std::string homeTag = "$HOME";
        if (value.rfind(homeTag, 0) == 0) return homeDir() / value.substr(homeTag.size() + (value.size() > homeTag.size() && value[homeTag.size()] == '/' ? 1 : 0));
        return fs::path(value);
    }
    fs::path p = homeDir() / fallback;
    if (fs::exists(p)) return p;
    return std::nullopt;
}

static unsigned int parseHexRGB(std::string s, unsigned int fallback) {
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6) return fallback;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (!end || *end != '\0' || v > 0xfffffful) return fallback;
    return (unsigned int)v;
}

static std::string hexRGB(Color c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

static Color accentFromHSVField(const std::string& text, Color fallback) {
    float h=0,sat=0,v=0;
    if(std::sscanf(text.c_str(), "%f,%f,%f", &h, &sat, &v)==3) {
        h=std::fmod(h,360.0f); if(h<0) h+=360.0f;
        sat=std::clamp(sat,0.0f,1.0f); v=std::clamp(v,0.0f,1.0f);
        return ColorFromHSV(h,sat,v);
    }
    unsigned rgb=parseHexRGB(text, (fallback.r<<16)|(fallback.g<<8)|fallback.b);
    return colorHex(rgb);
}

static Config loadConfig() {
    Config c;
    c.dir = configBase() / "raymothfm";
    c.file = c.dir / "config";

    std::ifstream in(c.file);
    if (!in) {
        // One-time compatibility with older development builds that used ~/.config/.raymothfm.
        const fs::path legacy = configBase() / ".raymothfm" / "config";
        in.open(legacy);
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "theme") c.theme = std::clamp(std::atoi(value.c_str()), 0, (int)kThemes.size()-1);
        else if (key == "accent") { c.accent = colorHex(parseHexRGB(value, (kThemes[c.theme].accent.r << 16) | (kThemes[c.theme].accent.g << 8) | kThemes[c.theme].accent.b)); c.accentCustom = true; }
        else if (key == "show_hidden") c.showHidden = std::atoi(value.c_str()) != 0;
        else if (key == "show_thumbnails") c.showThumbnails = std::atoi(value.c_str()) != 0;
        else if (key == "thumbnail_res") c.thumbnailRes = std::clamp(std::atoi(value.c_str()), 1, 5);
        else if (key == "font_scale") c.fontScale = std::clamp(std::strtof(value.c_str(), nullptr), 1.0f, 2.5f);
        else if (key == "editor" || key == "EDITOR") { c.editor = value; if(c.editor.size()>=2 && c.editor.front()=='"' && c.editor.back()=='"') c.editor=c.editor.substr(1,c.editor.size()-2); }
        else if (key == "term" || key == "TERM") { c.term = value; if(c.term.size()>=2 && c.term.front()=='"' && c.term.back()=='"') c.term=c.term.substr(1,c.term.size()-2); }
        else if (key == "favorite" && !value.empty()) c.favorites.push_back(value);
    }

    if (c.editor.empty()) { const char* env=std::getenv("EDITOR"); if(env && *env) c.editor=env; }

    if (c.favorites.empty()) {
        const std::array<std::pair<const char*, const char*>, 5> defaults = {{{"Desktop", "Desktop"}, {"Documents", "Documents"}, {"Downloads", "Downloads"}, {"Music", "Music"}, {"Pictures", "Pictures"}}};
        for (const auto& [_, fallback] : defaults) {
            fs::path p = homeDir() / fallback;
            if (fs::exists(p) && fs::is_directory(p)) c.favorites.push_back(p.string());
        }
        c.favorites.push_back(homeDir().string());
    }
    return c;
}

static void saveConfig(const Config& c, bool allowCreate = false) {
    std::error_code ec;
    if (!allowCreate && !fs::exists(c.file, ec)) return; // New configs are created only by --mkcfg.
    std::ofstream out(c.file);
    if (!out) return;
    out << "# raymothfm configuration\n";
    out << "theme=" << c.theme << "\n";
    out << "accent=" << hexRGB(c.accent) << "\n";
    out << "show_hidden=" << (c.showHidden ? 1 : 0) << "\n";
    out << "show_thumbnails=" << (c.showThumbnails ? 1 : 0) << "\n";
    out << "thumbnail_res=" << c.thumbnailRes << "\n";
    out << "font_scale=" << c.fontScale << "\n";
    out << "EDITOR=\"" << c.editor << "\"\n";
    out << "TERM=\"" << c.term << "\"\n";
    for (const auto& f : c.favorites) out << "favorite=" << f << "\n";
}

static bool writeTemplateConfig() {
    Config c;
    c.dir = configBase() / "raymothfm";
    c.file = c.dir / "config";
    std::error_code ec;
    fs::create_directories(c.dir, ec);
    if (ec) { std::fprintf(stderr, "raymothfm: cannot create config directory: %s\n", ec.message().c_str()); return false; }
    if (fs::exists(c.file, ec)) {
        std::fprintf(stderr, "raymothfm: config already exists: %s\n", c.file.c_str());
        return false;
    }
    c.editor = "";
    c.term = "foot";
    c.fontScale = 1.15f;
    c.favorites.clear();
    saveConfig(c, true);
    std::printf("raymothfm: created default config: %s\n", c.file.c_str());
    return true;
}

struct MagicInfo {
    std::string description;
    std::string mime;
};

class MagicDatabase {
public:
    MagicDatabase() {
        cookie_ = magic_open(MAGIC_MIME_TYPE);
        if (cookie_) {
            if (magic_load(cookie_, nullptr) != 0) {
                magic_close(cookie_);
                cookie_ = nullptr;
            }
        }
        desc_ = magic_open(MAGIC_NONE);
        if (desc_) {
            if (magic_load(desc_, nullptr) != 0) {
                magic_close(desc_);
                desc_ = nullptr;
            }
        }
    }
    ~MagicDatabase() {
        if (cookie_) magic_close(cookie_);
        if (desc_) magic_close(desc_);
    }
    MagicInfo file(const fs::path& p) const {
        MagicInfo out;
        if (desc_) {
            const char* s = magic_file(desc_, p.c_str());
            if (s) out.description = s;
        }
        if (cookie_) {
            const char* s = magic_file(cookie_, p.c_str());
            if (s) out.mime = s;
        }
        if (out.description.empty()) out.description = "Unknown file type";
        if (out.mime.empty()) out.mime = "application/octet-stream";
        return out;
    }
    bool available() const { return desc_ != nullptr; }
private:
    magic_t cookie_{nullptr};
    magic_t desc_{nullptr};
};

struct TextEditor {
    std::string* text = nullptr;
    size_t cursor = 0;
    size_t anchor = 0;
    bool hasSelection() const { return text && cursor != anchor; }
    size_t lo() const { return std::min(cursor, anchor); }
    size_t hi() const { return std::max(cursor, anchor); }
    void begin(std::string* t, bool all = false) { text = t; cursor = t ? t->size() : 0; anchor = cursor; if (all && t) anchor = 0; }
    void beginEmpty(std::string* t) { text=t; cursor=0; anchor=0; if(t) t->clear(); }
    void selectAll() { if (text) { anchor = 0; cursor = text->size(); } }
    void clear() { anchor = cursor; }
    void replace(const std::string& s) { if (!text) return; const size_t a = lo(), b = hi(); text->replace(a, b-a, s); cursor = a + s.size(); anchor = cursor; }
    void moveLeft(bool extendSelection) {
        if (!text) return;
        if (!extendSelection && hasSelection()) { cursor = lo(); anchor = cursor; return; }
        if (cursor) --cursor;
        if (!extendSelection) anchor = cursor;
    }
    void moveRight(bool extendSelection) {
        if (!text) return;
        if (!extendSelection && hasSelection()) { cursor = hi(); anchor = cursor; return; }
        if (cursor < text->size()) ++cursor;
        if (!extendSelection) anchor = cursor;
    }
    void backspace() { if (!text) return; if (hasSelection()) return replace(""); if (cursor) { text->erase(cursor-1,1); --cursor; anchor=cursor; } }
    void backspaceToSlash() { if (!text) return; if (hasSelection()) { replace(""); return; } if (!cursor) return; size_t slash=text->rfind('/', cursor-1); size_t start=(slash==std::string::npos)?0:slash+1; text->erase(start, cursor-start); cursor=start; anchor=cursor; }
    void del() { if (!text) return; if (hasSelection()) return replace(""); if (cursor < text->size()) text->erase(cursor,1); }
};

struct ArchiveOptions {
    ArchiveFormat format = ArchiveFormat::Tar;
    ArchiveCompression compression = ArchiveCompression::Gzip;
    int level = 6;
    int threads = 1;
    std::string output;
    std::string destination;
    std::string sevenZipMethod = "lzma2";
    bool overwrite = true;
    std::string containerName;
    std::string password;
    int encryption = 0; // 0 none, 1 zipcrypt, 2 aes128, 3 aes256
};

struct ClipboardBuffer {
    std::vector<fs::path> paths;
    bool cut = false;
};

struct PropertiesState {
    std::string path;
    MagicInfo magic;
    mode_t mode = 0644;
    std::string modeEdit = "0644";
    std::uintmax_t totalSize = 0;
    std::uint64_t itemCount = 0;
    std::uint64_t selectedCount = 1;
    bool multi = false;
    std::time_t mtime = 0;
    TextEditor editor;
    int field = -1;
};

struct DiskMountInfo { std::string source; fs::path mountpoint; std::string fstype; std::uint64_t total=0, used=0, free=0; };
struct DiskDialogState { std::vector<DiskMountInfo> mounts; int tab=0; int firstTab=0; };

struct ContextMenu {
    bool open = false;
    int row = -1;
    Vector2 pos{0,0};
    std::vector<MenuAction> actions;
};

struct TabPage {
    std::unique_ptr<VfsProvider> vfs;
    std::string path;
    std::vector<VfsEntry> rows;
    std::vector<int> visibleIndices;
    std::set<int> selection;
    int selected = -1;
    int anchorSelection = -1;
    int scroll = 0;
    ViewMode view = ViewMode::Details;
    std::string addressEdit;
    std::string searchEdit;
    std::vector<std::string> history;
    int historyIndex = -1;
};


struct PackedUiFlags {
    uint32_t sidebar:1;
    uint32_t newItemDirectory:1;
    uint32_t sortAscending:1;
    uint32_t configDirty:1;
    uint32_t checksumBusy:1;
    uint32_t archiveTemplateAuto:1;
    uint32_t imageFit:1;
    uint32_t imageDragging:1;
    uint32_t pendingPasteCut:1;
    uint32_t convertLossless:1;
    uint32_t convertStrip:1;
    uint32_t convertOutputAuto:1;
    uint32_t reserved:20;
    PackedUiFlags() : sidebar(1), newItemDirectory(1), sortAscending(1), configDirty(0), checksumBusy(0),
        archiveTemplateAuto(1), imageFit(1), imageDragging(0), pendingPasteCut(0), convertLossless(0),
        convertStrip(0), convertOutputAuto(1), reserved(0) {}
};

static_assert(sizeof(PackedUiFlags) == sizeof(uint32_t), "PackedUiFlags must remain one 32-bit word");

struct ExplorerState {
    PackedUiFlags flags{};
    std::unique_ptr<VfsProvider> vfs;
    std::string path;
    std::vector<VfsEntry> rows;
    std::vector<int> visibleIndices;
    std::set<int> selection;
    int selected = -1;
    int anchorSelection = -1;
    int scroll = 0;
    ViewMode view = ViewMode::Details;
    std::string status;
    std::string addressEdit;
    std::string searchEdit;
    TextField focusedField = TextField::None;
    TextEditor editor;
    std::vector<std::string> history;
    int historyIndex = -1;
    float sidebarW = 240.0f;
    Modal modal = Modal::None;
    ArchiveOptions archive;
    int modalField = 0;
    std::string modalError;
    std::string editorEdit;
    std::string termEdit;
    std::string accentEdit;
    std::string fontScaleEdit;
    std::string renameEdit;
    std::string newItemEdit;
    PropertiesState props;
    DiskDialogState diskInfo;
    ContextMenu menu;
    ClipboardBuffer clipboard;
    Config config;
    MagicDatabase magic;
    SortKey sortKey = SortKey::Name;
    std::vector<TabPage> tabs;
    int activeTab = 0;
    std::string catText;
    int catScroll = 0;
    std::string commandEdit;
    std::string themeEdit;
    pid_t commandPid = -1;
    int commandFd = -1;
    std::string commandOutput;
    std::string checksumPath;
    std::string checksumName;
    std::string checksumValue;
    std::string archiveAutoSuffix;
    std::vector<std::string> completionCandidates;
    size_t completionIndex = 0;
    size_t completionTokenStart = 0;
    std::string completionSeed;
    float imageZoom = 1.0f;
    Vector2 imagePan{0,0};
    Vector2 imageDragStart{0,0};
    Vector2 imagePanStart{0,0};
    Texture2D imageTexture{};
    int imageW = 0, imageH = 0;
    std::string imagePath;
    std::unordered_map<std::string, Texture2D> thumbnailCache;
    std::unordered_map<std::string, uint64_t> thumbnailUse;
    std::unordered_set<std::string> thumbnailFailed;
    uint64_t thumbnailUseCounter = 0;
    std::unordered_set<std::string> thumbnailProtected;
    fs::path pendingPasteSource;
    fs::path pendingPasteDestination;
    std::vector<fs::path> pendingPermanentDelete;
    size_t pendingPasteIndex = 0;
    std::string convertInput;
    std::string convertOutput;
    std::vector<std::string> convertFormats;
    int convertFormat = 0;
    int convertQuality = 90;
    int convertEffort = 5;
    int convertCompression = 6;
    int convertBitDepth = 8;
    bool selectionDragging = false;
    bool selectionDragMoved = false;
    bool selectionDragAdditive = false;
    Vector2 selectionDragStart{0,0};
    Vector2 selectionDragCurrent{0,0};
    std::set<int> selectionDragBase;
};

static void unfocus(ExplorerState& s);
static void refresh(ExplorerState& s);
static int visibleRowsFor(ViewMode v,int h);
static void selectSingle(ExplorerState& s, int row, bool toggle);
static void selectRange(ExplorerState& s, int row);
static bool isImagePath(const std::string& name);
static void openImageViewer(ExplorerState& s);
static void openConvertImage(ExplorerState& s);

static TabPage saveTabSnapshot(ExplorerState& s) {
    TabPage p;
    p.vfs = std::move(s.vfs); p.path=std::move(s.path); p.rows=std::move(s.rows); p.visibleIndices=std::move(s.visibleIndices);
    p.selection=std::move(s.selection); p.selected=s.selected; p.anchorSelection=s.anchorSelection; p.scroll=s.scroll; p.view=s.view;
    p.addressEdit=std::move(s.addressEdit); p.searchEdit=std::move(s.searchEdit); p.history=std::move(s.history); p.historyIndex=s.historyIndex;
    return p;
}

static void restoreTabSnapshot(ExplorerState& s, TabPage&& p) {
    s.vfs=std::move(p.vfs); s.path=std::move(p.path); s.rows=std::move(p.rows); s.visibleIndices=std::move(p.visibleIndices);
    s.selection=std::move(p.selection); s.selected=p.selected; s.anchorSelection=p.anchorSelection; s.scroll=p.scroll; s.view=p.view;
    s.addressEdit=std::move(p.addressEdit); s.searchEdit=std::move(p.searchEdit); s.history=std::move(p.history); s.historyIndex=p.historyIndex;
    unfocus(s); s.menu.open=false;
}

static void persistActiveTab(ExplorerState& s) {
    if (s.tabs.empty() || s.activeTab<0 || s.activeTab>=(int)s.tabs.size()) return;
    s.tabs[s.activeTab]=saveTabSnapshot(s);
}

static void activateTab(ExplorerState& s, int index) {
    if (s.tabs.empty()) return;
    index=(index+(int)s.tabs.size())%(int)s.tabs.size();
    if (index==s.activeTab) return;
    persistActiveTab(s);
    s.activeTab=index;
    restoreTabSnapshot(s,std::move(s.tabs[index]));
}

static void newTab(ExplorerState& s, const fs::path& p) {
    persistActiveTab(s);
    TabPage page;
    std::error_code ec;
    if (fs::is_directory(p,ec)) {
        page.vfs=std::make_unique<LocalVfs>(p,s.config.showHidden);
        page.path=p.string(); page.addressEdit=page.path; page.history={page.path}; page.historyIndex=0;
        page.rows=page.vfs->list(page.path);
    }
    page.view=s.view;
    s.tabs.push_back(std::move(page));
    s.activeTab=(int)s.tabs.size()-1;
    restoreTabSnapshot(s,std::move(s.tabs[s.activeTab]));
    refresh(s);
}

static void closeActiveTab(ExplorerState& s) {
    if (s.tabs.empty() || s.tabs.size()==1) { s.status="Cannot close the last tab"; return; }
    persistActiveTab(s);
    s.tabs.erase(s.tabs.begin()+s.activeTab);
    s.activeTab=std::min(s.activeTab,(int)s.tabs.size()-1);
    restoreTabSnapshot(s,std::move(s.tabs[s.activeTab]));
}

static std::string tabTitle(const TabPage& p) {
    if (!p.path.empty()) { fs::path x(p.path); auto n=x.filename().string(); if(!n.empty()) return n; return x.string(); }
    return "Tab";
}

static std::string tabTitle(const ExplorerState& s, int index) {
    if (index==s.activeTab) {
        if (!s.path.empty()) { fs::path x(s.path); auto n=x.filename().string(); return n.empty()?x.string():n; }
        return "Tab";
    }
    if (index>=0 && index<(int)s.tabs.size()) return tabTitle(s.tabs[index]);
    return "Tab";
}

static std::string archiveFormatName(ArchiveFormat f) {
    switch (f) { case ArchiveFormat::Tar: return "tar"; case ArchiveFormat::Pax: return "pax"; case ArchiveFormat::Ustar: return "ustar"; case ArchiveFormat::Zip: return "zip"; case ArchiveFormat::SevenZip: return "7z"; case ArchiveFormat::Cpio: return "cpio"; }
    return "tar";
}
static std::string compressionName(ArchiveCompression c) {
    switch (c) { case ArchiveCompression::None: return "none"; case ArchiveCompression::Gzip: return "gzip"; case ArchiveCompression::Bzip2: return "bzip2"; case ArchiveCompression::Xz: return "xz"; case ArchiveCompression::Zstd: return "zstd"; case ArchiveCompression::Lz4: return "lz4"; case ArchiveCompression::Lzma: return "lzma"; case ArchiveCompression::Lzip: return "lzip"; }
    return "none";
}
static std::string defaultArchiveSuffix(const ArchiveOptions& o) {
    const std::string f = archiveFormatName(o.format);
    if (f == "7z") return ".7z";
    if (f == "zip") return ".zip";
    if (f == "cpio") return ".cpio";
    if (o.compression == ArchiveCompression::Gzip) return ".tar.gz";
    if (o.compression == ArchiveCompression::Bzip2) return ".tar.bz2";
    if (o.compression == ArchiveCompression::Xz) return ".tar.xz";
    if (o.compression == ArchiveCompression::Zstd) return ".tar.zst";
    if (o.compression == ArchiveCompression::Lz4) return ".tar.lz4";
    if (o.compression == ArchiveCompression::Lzma) return ".tar.lzma";
    if (o.compression == ArchiveCompression::Lzip) return ".tar.lz";
    return ".tar";
}

static bool isSafeArchivePath(const fs::path& root, const fs::path& rel) {
    if (rel.empty() || rel.is_absolute()) return false;
    fs::path normalized = rel.lexically_normal();
    for (const auto& part : normalized) if (part == "..") return false;
    const fs::path candidate = (root / normalized).lexically_normal();
    const std::string r = root.lexically_normal().string();
    const std::string c = candidate.string();
    return c == r || (c.size() > r.size() && c.rfind(r + '/', 0) == 0);
}

static bool setupArchiveWriter(archive* a, const ArchiveOptions& o, std::string& error) {
    int r = ARCHIVE_OK;
    if (o.format == ArchiveFormat::Tar) r = archive_write_set_format_pax(a);
    else if (o.format == ArchiveFormat::Pax) r = archive_write_set_format_pax_restricted(a);
    else if (o.format == ArchiveFormat::Ustar) r = archive_write_set_format_ustar(a);
    else if (o.format == ArchiveFormat::Zip) r = archive_write_set_format_zip(a);
    else if (o.format == ArchiveFormat::SevenZip) r = archive_write_set_format_7zip(a);
    else if (o.format == ArchiveFormat::Cpio) r = archive_write_set_format_cpio(a);
    if (r != ARCHIVE_OK) { error = archive_error_string(a) ? archive_error_string(a) : "Unable to select archive format"; return false; }

    if (o.format == ArchiveFormat::SevenZip) {
        if (!o.password.empty()) { error = "Encrypted 7z creation uses the system 7z tool; libarchive itself cannot write encrypted 7z."; return false; }
        if (archive_write_set_option(a, "7zip", "compression", o.sevenZipMethod.c_str()) != ARCHIVE_OK ||
            archive_write_set_option(a, "7zip", "compression-level", std::to_string(o.level).c_str()) != ARCHIVE_OK) {
            error = archive_error_string(a) ? archive_error_string(a) : "Unable to configure 7z";
            return false;
        }
        // Some libarchive builds do not expose the 7zip thread option.
        // Treat that as an optional capability rather than failing archive creation.
        (void)archive_write_set_option(a, "7zip", "threads", std::to_string(std::max(1, o.threads)).c_str());
        return true;
    }
    if (o.format == ArchiveFormat::Zip) {
        r = o.compression == ArchiveCompression::None ? archive_write_zip_set_compression_store(a) : archive_write_zip_set_compression_deflate(a);
        if (r != ARCHIVE_OK) { error = archive_error_string(a) ? archive_error_string(a) : "Unable to configure zip"; return false; }
        if (!o.password.empty()) {
            const char* enc = o.encryption==2 ? "aes128" : (o.encryption==3 ? "aes256" : "zipcrypt");
            if (archive_write_set_format_option(a, "zip", "encryption", enc) != ARCHIVE_OK || archive_write_set_passphrase(a, o.password.c_str()) != ARCHIVE_OK) {
                error = archive_error_string(a) ? archive_error_string(a) : "Unable to configure ZIP encryption"; return false;
            }
        }
        return true;
    }
    switch (o.compression) {
        case ArchiveCompression::None: r = archive_write_add_filter_none(a); break;
        case ArchiveCompression::Gzip: r = archive_write_add_filter_gzip(a); break;
        case ArchiveCompression::Bzip2: r = archive_write_add_filter_bzip2(a); break;
        case ArchiveCompression::Xz: r = archive_write_add_filter_xz(a); break;
        case ArchiveCompression::Zstd: r = archive_write_add_filter_zstd(a); break;
        case ArchiveCompression::Lz4: r = archive_write_add_filter_lz4(a); break;
        case ArchiveCompression::Lzma: r = archive_write_add_filter_lzma(a); break;
        case ArchiveCompression::Lzip: r = archive_write_add_filter_lzip(a); break;
    }
    if (r != ARCHIVE_OK) { error = archive_error_string(a) ? archive_error_string(a) : "Unable to add compression filter"; return false; }
    const std::string level = std::to_string(o.level);
    if (o.compression != ArchiveCompression::None) {
        const std::string filter = compressionName(o.compression);
        if (archive_write_set_filter_option(a, filter.c_str(), "compression-level", level.c_str()) == ARCHIVE_FATAL) {
            error = archive_error_string(a) ? archive_error_string(a) : "Unable to set compression level"; return false;
        }
    }
    return true;
}

static bool addFsEntry(archive* a, const fs::path& rootBase, const fs::path& source, std::string& error) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(source, ec);
    if (ec) { error = "Cannot stat " + source.string() + ": " + ec.message(); return false; }

    const fs::path rel = fs::relative(source, rootBase, ec);
    if (ec || rel.empty()) { error = "Cannot make relative archive path for " + source.string(); return false; }

    archive_entry* e = archive_entry_new();
    if (!e) { error = "archive_entry_new failed"; return false; }
    archive_entry_set_pathname(e, rel.generic_string().c_str());
    const auto ft = fs::last_write_time(source, ec);
    if (!ec) archive_entry_set_mtime(e, fileTimeToTimeT(ft), 0);

    if (fs::is_symlink(st)) {
        const fs::path target = fs::read_symlink(source, ec);
        if (ec) { archive_entry_free(e); error = ec.message(); return false; }
        archive_entry_set_filetype(e, AE_IFLNK);
        archive_entry_set_symlink(e, target.generic_string().c_str());
        archive_entry_set_perm(e, 0777);
        const int hr = archive_write_header(a, e);
        archive_entry_free(e);
        if (hr < ARCHIVE_WARN) { error = archive_error_string(a) ? archive_error_string(a) : "Cannot write symlink"; return false; }
        return true;
    }

    if (fs::is_directory(st)) {
        const auto perms = fs::status(source, ec).permissions();
        mode_t mode = 0755;
        if (!ec) mode = static_cast<mode_t>(perms & fs::perms::all);
        archive_entry_set_filetype(e, AE_IFDIR);
        archive_entry_set_perm(e, mode);
        const int hr = archive_write_header(a, e);
        archive_entry_free(e);
        if (hr < ARCHIVE_WARN) { error = archive_error_string(a) ? archive_error_string(a) : "Cannot write directory"; return false; }

        fs::directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), endIt;
        if (ec) { error = "Cannot enumerate " + source.string() + ": " + ec.message(); return false; }
        for (; it != endIt; it.increment(ec)) {
            if (ec) { error = "Cannot enumerate " + source.string() + ": " + ec.message(); return false; }
            if (!addFsEntry(a, rootBase, it->path(), error)) return false;
        }
        return true;
    }

    if (!fs::is_regular_file(st)) return true;

    const auto size = fs::file_size(source, ec);
    if (ec) { archive_entry_free(e); error = "Cannot size " + source.string() + ": " + ec.message(); return false; }
    auto perms = fs::status(source, ec).permissions();
    mode_t mode = 0644;
    if (!ec) mode = static_cast<mode_t>(perms & fs::perms::all);
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_size(e, static_cast<la_int64_t>(size));
    archive_entry_set_perm(e, mode);
    if (archive_write_header(a, e) < ARCHIVE_WARN) {
        error = archive_error_string(a) ? archive_error_string(a) : "Cannot write file header";
        archive_entry_free(e);
        return false;
    }

    std::ifstream in(source, std::ios::binary);
    if (!in) { archive_entry_free(e); error = "Cannot read " + source.string(); return false; }
    char buffer[128*1024];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        const la_ssize_t wr = archive_write_data(a, buffer, static_cast<size_t>(got));
        if (wr < 0 || wr != got) {
            error = archive_error_string(a) ? archive_error_string(a) : "Archive write failed";
            archive_entry_free(e);
            return false;
        }
    }
    archive_entry_free(e);
    return true;
}

static std::string shellQuote(const std::string& in);

static bool commandExists(const char* name) { return std::system((std::string("command -v ")+shellQuote(name)+" >/dev/null 2>&1").c_str()) == 0; }

static bool createEncrypted7zExternal(const std::vector<fs::path>& sources, const fs::path& base, const fs::path& output, const ArchiveOptions& o, std::string& error) {
    if (o.password.empty()) return false;
    if (!commandExists("7z") && !commandExists("7zz")) { error = "Encrypted 7z requires the 7z/7zz system tool."; return false; }
    const char* exe = commandExists("7z") ? "7z" : "7zz";
    std::ostringstream cmd;
    cmd << shellQuote(exe) << " a -y -p" << shellQuote(o.password) << " -mhe=on";
    if (!o.sevenZipMethod.empty()) cmd << " -m0=" << shellQuote(o.sevenZipMethod);
    if (o.level >= 0) cmd << " -mx=" << std::clamp(o.level,0,9);
    if (o.threads > 0) cmd << " -mmt=" << std::clamp(o.threads,1,64);
    cmd << " " << shellQuote(output.string());
    for (const auto& p : sources) {
        fs::path rel; std::error_code ec; rel=fs::relative(p,base,ec);
        cmd << " " << shellQuote((ec? p : rel).generic_string());
    }
    pid_t pid=fork(); if(pid<0) { error=std::strerror(errno); return false; }
    if(pid==0){ if(chdir(base.c_str())!=0)_exit(126); execl("/bin/sh","sh","-lc",cmd.str().c_str(),(char*)nullptr); _exit(127); }
    int status=0; if(waitpid(pid,&status,0)<0){ error=std::strerror(errno); return false; }
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){ error="7z encryption command failed"; return false; }
    return true;
}

static bool createArchive(const std::vector<fs::path>& sources, const fs::path& base, const fs::path& output, const ArchiveOptions& o, std::string& error) {
    if (sources.empty()) { error = "Nothing selected"; return false; }
    if (o.format == ArchiveFormat::SevenZip && !o.password.empty()) return createEncrypted7zExternal(sources, base, output, o, error);
    std::error_code ec;
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path(), ec);
    archive* a = archive_write_new();
    if (!a) { error = "archive_write_new failed"; return false; }
    if (!setupArchiveWriter(a, o, error)) { archive_write_free(a); return false; }
    if (archive_write_open_filename(a, output.string().c_str()) != ARCHIVE_OK) { error = archive_error_string(a) ? archive_error_string(a) : "Cannot open output archive"; archive_write_free(a); return false; }
    for (const auto& p : sources) {
        if (!addFsEntry(a, base, p, error)) { archive_write_close(a); archive_write_free(a); return false; }
    }
    const int closeResult = archive_write_close(a);
    const int freeResult = archive_write_free(a);
    return closeResult == ARCHIVE_OK && freeResult == ARCHIVE_OK;
}


static bool isSevenZipArchivePath(const fs::path& p) {
    return lowerCopy(p.extension().string()) == ".7z";
}

static bool extractSevenZipExternal(const fs::path& archivePath, const fs::path& destination,
                                    bool overwrite, const std::string& password, std::string& error) {
    if (!commandExists("7z") && !commandExists("7zz")) {
        error = "7z password extraction requires the 7z/7zz system tool.";
        return false;
    }
    const char* exe = commandExists("7z") ? "7z" : "7zz";
    std::ostringstream cmd;
    cmd << shellQuote(exe) << " x ";
    cmd << (overwrite ? "-y " : "-aos ");
    if (!password.empty()) cmd << "-p" << shellQuote(password) << " ";
    cmd << "-o" << shellQuote(destination.string()) << " " << shellQuote(archivePath.string());
    pid_t pid=fork();
    if(pid<0){ error=std::strerror(errno); return false; }
    if(pid==0){
        execl("/bin/sh","sh","-lc",cmd.str().c_str(),(char*)nullptr);
        _exit(127);
    }
    int status=0;
    if(waitpid(pid,&status,0)<0){ error=std::strerror(errno); return false; }
    if(WIFEXITED(status) && WEXITSTATUS(status)==0) return true;
    error = password.empty() ? "PASSWORD_REQUIRED:7z archive needs a password" : "PASSWORD_REQUIRED:Incorrect 7z password or extraction failed";
    return false;
}

static bool extractArchive(const fs::path& archivePath, const fs::path& destination, bool overwrite, const std::string& password, std::string& error) {
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) { error = "Cannot create extraction directory: " + ec.message(); return false; }
    if (isSevenZipArchivePath(archivePath) && !password.empty()) {
        return extractSevenZipExternal(archivePath, destination, overwrite, password, error);
    }
    archive* a = archive_read_new();
    if (!a) { error = "archive_read_new failed"; return false; }
    archive_read_support_filter_all(a); archive_read_support_format_all(a);
    if (!password.empty()) archive_read_add_passphrase(a, password.c_str());
    if (archive_read_open_filename(a, archivePath.string().c_str(), 10240) != ARCHIVE_OK) { error = archive_error_string(a) ? archive_error_string(a) : "Cannot open archive"; archive_read_free(a); return false; }
    archive* disk = archive_write_disk_new();
    if (!disk) { archive_read_free(a); error = "archive_write_disk_new failed"; return false; }
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS | ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT;
    if (!overwrite) flags |= ARCHIVE_EXTRACT_NO_OVERWRITE;
    archive_write_disk_set_options(disk, flags); archive_write_disk_set_standard_lookup(disk);
    archive_entry* entry = nullptr;
    int headerResult = ARCHIVE_OK;
    while ((headerResult = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char* name = archive_entry_pathname(entry);
        if (!name) { archive_read_data_skip(a); continue; }
        const fs::path rel = fs::path(name).lexically_normal();
        if (!isSafeArchivePath(destination, rel)) { error = "Blocked unsafe archive path: " + rel.string(); archive_write_free(disk); archive_read_free(a); return false; }
        const fs::path full = destination / rel;
        archive_entry_set_pathname(entry, full.string().c_str());
        if (archive_write_header(disk, entry) < ARCHIVE_WARN) { error = archive_error_string(disk) ? archive_error_string(disk) : "Extraction header failed"; archive_write_free(disk); archive_read_free(a); return false; }
        if (archive_entry_size(entry) > 0) {
            char buffer[128*1024]; la_ssize_t n;
            while ((n = archive_read_data(a, buffer, sizeof(buffer))) > 0) if (archive_write_data(disk, buffer, (size_t)n) < 0) { error = archive_error_string(disk) ? archive_error_string(disk) : "Extraction write failed"; archive_write_free(disk); archive_read_free(a); return false; }
            if (n < ARCHIVE_OK) {
                const char* msg = archive_error_string(a);
                error = msg ? msg : "Archive data could not be decrypted";
                if (error.find("passphrase") != std::string::npos || error.find("Passphrase") != std::string::npos || error.find("password") != std::string::npos || error.find("Password") != std::string::npos || !password.empty()) error = "PASSWORD_REQUIRED:" + error;
                archive_write_free(disk); archive_read_free(a); return false;
            }
        }
        if (archive_write_finish_entry(disk) != ARCHIVE_OK) { error = archive_error_string(disk) ? archive_error_string(disk) : "Extraction finish failed"; archive_write_free(disk); archive_read_free(a); return false; }
    }
    std::string readError = archive_error_string(a) ? archive_error_string(a) : "Archive password required or extraction failed";
    const int rr = archive_read_close(a); archive_read_free(a);
    const int dr = archive_write_close(disk); archive_write_free(disk);
    if (headerResult != ARCHIVE_EOF) {
        error = readError;
        if (error.empty()) error = "Archive password required or extraction failed";
        const bool looksPassword = error.find("passphrase") != std::string::npos || error.find("Passphrase") != std::string::npos || error.find("password") != std::string::npos || error.find("Password") != std::string::npos;
        if (isSevenZipArchivePath(archivePath) && password.empty()) {
            error = "PASSWORD_REQUIRED:7z archive may be password protected";
        } else if (looksPassword) {
            error = "PASSWORD_REQUIRED:" + error;
        }
        return false;
    }
    if (rr != ARCHIVE_OK || dr != ARCHIVE_OK) { error = "Archive extraction did not finish cleanly"; return false; }
    return true;
}

static std::string sortType(const VfsEntry& e) {
    if (e.kind==EntryKind::Directory) return "folder";
    if (e.kind==EntryKind::Archive) return "archive";
    const auto dot=lowerCopy(e.name).find_last_of('.');
    return dot==std::string::npos?"file":lowerCopy(e.name).substr(dot+1);
}

static void applySort(ExplorerState& s) {
    auto cmp=[&](const VfsEntry& a,const VfsEntry& b){
        if ((a.kind==EntryKind::Directory)!=(b.kind==EntryKind::Directory)) return a.kind==EntryKind::Directory;
        bool less=false, greater=false;
        switch(s.sortKey){
            case SortKey::Name: {auto aa=lowerCopy(a.name),bb=lowerCopy(b.name);less=aa<bb;greater=aa>bb;break;}
            case SortKey::Size: less=a.size<b.size;greater=a.size>b.size;break;
            case SortKey::Date: less=a.mtime<b.mtime;greater=a.mtime>b.mtime;break;
            case SortKey::Type: {auto aa=sortType(a),bb=sortType(b);less=aa<bb;greater=aa>bb;break;}
        }
        if (less||greater) return s.flags.sortAscending?less:greater;
        return lowerCopy(a.name)<lowerCopy(b.name);
    };
    std::sort(s.rows.begin(),s.rows.end(),cmp);
}

static void updateFilter(ExplorerState& s) {
    s.visibleIndices.clear();
    for (int i=0;i<(int)s.rows.size();++i) if (fuzzyMatch(s.rows[i].name, s.searchEdit)) s.visibleIndices.push_back(i);
    s.scroll = 0;
    s.selection.clear(); s.selected = -1; s.anchorSelection = -1;
    s.status = std::to_string(s.visibleIndices.size()) + " items" + (s.searchEdit.empty() ? "" : " (filtered)");
}

static void refresh(ExplorerState& s) {
    if (!s.vfs) return;
    s.thumbnailFailed.clear();
    s.vfs->setShowHidden(s.config.showHidden);
    s.rows = s.vfs->list(s.path);
    applySort(s);
    updateFilter(s);
}

static void unfocus(ExplorerState& s) { s.focusedField = TextField::None; s.editor.text = nullptr; }
static void focus(ExplorerState& s, TextField f, bool all=false) {
    s.focusedField = f;
    if (f == TextField::Address) s.editor.begin(&s.addressEdit, all);
    else if (f == TextField::Search) s.editor.begin(&s.searchEdit, all);
    else if (f == TextField::Rename) s.editor.begin(&s.renameEdit, all);
    else if (f == TextField::NewItem) s.editor.begin(&s.newItemEdit, all);
    else if (f == TextField::Mode) s.editor.begin(&s.props.modeEdit, all);
    else if (f == TextField::ArchiveOutput) s.editor.begin(&s.archive.output, all);
    else if (f == TextField::ConvertOutput) s.editor.begin(&s.convertOutput, all);
    else if (f == TextField::ExtractDestination) s.editor.begin(&s.archive.destination, all);
    else if (f == TextField::ArchivePassword) s.editor.begin(&s.archive.password, all);
    else if (f == TextField::Command) s.editor.begin(&s.commandEdit, all);
    else if (f == TextField::ThemeNumber) s.editor.begin(&s.themeEdit, all);
    else if (f == TextField::EditorConfig) s.editor.begin(&s.editorEdit, all);
    else if (f == TextField::TermConfig) s.editor.begin(&s.termEdit, all);
    else if (f == TextField::AccentConfig) s.editor.begin(&s.accentEdit, all);
    else if (f == TextField::FontScale) s.editor.begin(&s.fontScaleEdit, all);
}
static void focusAt(ExplorerState& s, TextField f, float mouseX, float leftOffset, int fontSize=14) {
    focus(s,f,false);
    if (!s.editor.text) return;
    std::string& v=*s.editor.text;
    float target=mouseX-leftOffset;
    size_t best=0; int bestD=INT_MAX;
    for(size_t i=0;i<=v.size();++i){ int w=MeasureText(v.substr(0,i).c_str(),uiFont(fontSize)); int d=std::abs((int)target-w); if(d<bestD){bestD=d;best=i;} }
    s.editor.cursor=best; s.editor.anchor=best;
}


static void resetCompletion(ExplorerState& s) {
    s.completionCandidates.clear();
    s.completionIndex = 0;
    s.completionTokenStart = 0;
    s.completionSeed.clear();
}

static std::vector<std::string> pathCompletions(const std::string& token) {
    std::vector<std::string> out;
    fs::path raw(token.empty() ? "." : token);
    fs::path dir = raw.parent_path();
    std::string prefix = raw.filename().string();
    if (dir.empty()) dir = ".";
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path name = it->path().filename();
        const std::string n = name.string();
        if (!prefix.empty() && lowerCopy(n).rfind(lowerCopy(prefix), 0) != 0) continue;
        fs::path candidate = dir / name;
        std::string value = candidate.string();
        if (!token.empty() && token[0] != '/' && value.rfind("./",0)==0) value.erase(0,2);
        if (fs::is_directory(it->symlink_status(ec))) value += "/";
        out.push_back(value);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a,const std::string& b){return lowerCopy(a)<lowerCopy(b);});
    return out;
}

static std::vector<std::string> commandCompletions(const std::string& token) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return out;
    std::string paths(pathEnv);
    size_t pos=0;
    while (pos<=paths.size()) {
        const size_t sep=paths.find(':',pos);
        const std::string d=paths.substr(pos, sep==std::string::npos?std::string::npos:sep-pos);
        fs::path dir=d.empty()?".":fs::path(d);
        std::error_code ec;
        for (fs::directory_iterator it(dir,fs::directory_options::skip_permission_denied,ec),end; !ec && it!=end; it.increment(ec)) {
            const std::string n=it->path().filename().string();
            if ((!token.empty() && n.rfind(token,0)!=0) || n.empty() || seen.count(n)) continue;
            std::error_code sec; if (fs::is_regular_file(it->symlink_status(sec)) || fs::is_symlink(it->symlink_status(sec))) {
                if (::access(it->path().c_str(),X_OK)==0) { seen.insert(n); out.push_back(n); }
            }
        }
        if (sep==std::string::npos) break;
        pos=sep+1;
    }
    std::sort(out.begin(),out.end());
    return out;
}

static void completeFocusedText(ExplorerState& s) {
    if (!s.editor.text || (s.focusedField!=TextField::Address && s.focusedField!=TextField::Command)) return;
    std::string& text=*s.editor.text;
    const size_t cursor=s.editor.cursor;
    size_t tokenStart=cursor;
    if (s.focusedField==TextField::Address) {
        tokenStart=0;
    } else {
        while (tokenStart>0 && !std::isspace((unsigned char)text[tokenStart-1])) --tokenStart;
    }
    const std::string token=text.substr(tokenStart,cursor-tokenStart);
    if (s.completionCandidates.empty() || s.completionSeed!=token || s.completionTokenStart!=tokenStart) {
        s.completionSeed=token; s.completionTokenStart=tokenStart; s.completionIndex=0;
        s.completionCandidates = (s.focusedField==TextField::Address) ? pathCompletions(token) : commandCompletions(token);
        if (s.completionCandidates.empty() && s.focusedField==TextField::Command && token.find('/')!=std::string::npos)
            s.completionCandidates=pathCompletions(token);
    } else {
        s.completionIndex=(s.completionIndex+1)%s.completionCandidates.size();
    }
    if (s.completionCandidates.empty()) return;
    std::string replacement=s.completionCandidates[s.completionIndex];
    if (s.focusedField==TextField::Command && tokenStart>0 && text[tokenStart-1]==' ') {
        // no-op; preserve separation
    }
    s.editor.cursor=tokenStart;
    s.editor.anchor=cursor;
    s.editor.replace(replacement);
    s.editor.clear();
}

static int gridColumns(const ExplorerState& s) {
    if (s.view != ViewMode::MediumIcons && s.view != ViewMode::LargeIcons) return 1;
    const int W = GetScreenWidth();
    const int contentW = std::max(1, static_cast<int>(W - s.sidebarW));
    const int cellW = s.view == ViewMode::MediumIcons ? 135 : 180;
    return std::max(1, contentW / cellW);
}

static void moveSelection(ExplorerState& s, int delta, bool page=false) {
    if (s.visibleIndices.empty()) return;
    int pos=0;
    auto it=std::find(s.visibleIndices.begin(),s.visibleIndices.end(),s.selected);
    if (it!=s.visibleIndices.end()) pos=(int)std::distance(s.visibleIndices.begin(),it);

    int actualDelta = delta;
    if (s.view == ViewMode::MediumIcons || s.view == ViewMode::LargeIcons) {
        const int cols = gridColumns(s);
        // For icon views, callers pass row/column direction: +/-1 means left/right;
        // +/-cols means up/down. Page movement still advances by whole rows.
        if (page) {
            const int rows = std::max(1, visibleRowsFor(s.view, GetScreenHeight()-120));
            actualDelta = delta * cols * std::max(1, rows - 1);
        }
    } else if (page) {
        const int visible=visibleRowsFor(s.view, GetScreenHeight()-120);
        actualDelta = delta * std::max(1,visible-1);
    }

    pos=std::clamp(pos+actualDelta,0,(int)s.visibleIndices.size()-1);
    const int idx=s.visibleIndices[pos];
    if (isShiftDown() && s.anchorSelection>=0) selectRange(s,idx); else selectSingle(s,idx,false);
    const int visible=visibleRowsFor(s.view, GetScreenHeight()-120);
    if (pos<s.scroll) s.scroll=pos;
    if (pos>=s.scroll+visible) s.scroll=pos-visible+1;
}

static void openHelp(ExplorerState& s) { s.modal=Modal::Help; s.menu.open=false; unfocus(s); }
static void openAbout(ExplorerState& s) { s.modal=Modal::About; s.menu.open=false; unfocus(s); }

static std::string currentDisplayPath(const ExplorerState& s) {
    // LocalVfs owns the provider instance created for a directory, so its displayPath()
    // can become stale after navigating within that provider. The Explorer's current
    // path is authoritative for local filesystems; archive providers need their own
    // display path because they carry the virtual archive location.
    return (s.vfs && !s.vfs->isLocal()) ? s.vfs->displayPath() : s.path;
}

static void navigate(ExplorerState& s, std::string p, bool pushHistory = true) {
    if (!s.vfs || !s.vfs->isDirectory(p)) return;
    if (pushHistory) {
        if (s.historyIndex + 1 < (int)s.history.size()) s.history.resize((size_t)s.historyIndex + 1);
        s.history.push_back(p); s.historyIndex = (int)s.history.size()-1;
    }
    s.path = std::move(p); s.addressEdit = s.path; s.searchEdit.clear(); unfocus(s); s.menu.open=false; refresh(s);
}

static void openLocalPath(ExplorerState& s, const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) { s.status = "Not a directory: " + p.string(); return; }
    s.vfs = std::make_unique<LocalVfs>(p, s.config.showHidden);
    navigate(s, p.string(), true);
}

static bool isSingleCompressionFile(const fs::path& p);
static void extractSingleCompressionFile(ExplorerState& s, const fs::path& p, bool openResult = true);

static void openArchive(ExplorerState& s, const fs::path& p) {
    // Standalone .gz/.zst streams are not archive containers.
    // Only true containers (e.g. .tar.gz/.tar.zst) go through libarchive.
    if (isSingleCompressionFile(p)) {
        extractSingleCompressionFile(s, p, true);
        return;
    }
    auto av = std::make_unique<ArchiveVfs>(p.string(), s.config.showHidden);
    if (!av->valid()) { s.status = av->error(); return; }
    s.vfs = std::move(av); navigate(s, "/", true);
}

static std::vector<int> selectedRows(const ExplorerState& s) {
    std::vector<int> out(s.selection.begin(), s.selection.end());
    if (out.empty() && s.selected >= 0) out.push_back(s.selected);
    return out;
}

static void selectSingle(ExplorerState& s, int row, bool toggle=false) {
    if (row < 0 || row >= (int)s.rows.size()) return;
    if (!toggle) s.selection.clear();
    if (toggle && s.selection.count(row)) s.selection.erase(row); else s.selection.insert(row);
    s.selected=row; s.anchorSelection=row;
}

static void selectRange(ExplorerState& s, int row) {
    if (row < 0 || row >= (int)s.rows.size()) return;
    if (s.anchorSelection < 0) return selectSingle(s,row,false);
    s.selection.clear();
    const int lo = std::min(s.anchorSelection,row), hi=std::max(s.anchorSelection,row);
    for (int i=lo;i<=hi;++i) s.selection.insert(i);
    s.selected=row;
}

static void selectAll(ExplorerState& s) {
    s.selection.clear();
    for (int i : s.visibleIndices) s.selection.insert(i);
    if (!s.visibleIndices.empty()) { s.selected=s.visibleIndices.front(); s.anchorSelection=s.selected; }
}

static Rectangle normalizedRect(Vector2 a, Vector2 b) {
    const float x=std::min(a.x,b.x), y=std::min(a.y,b.y);
    return {x,y,std::fabs(b.x-a.x),std::fabs(b.y-a.y)};
}

static bool rectsOverlap(Rectangle a, Rectangle b) {
    return a.x < b.x+b.width && a.x+a.width > b.x && a.y < b.y+b.height && a.y+a.height > b.y;
}

static void updateDragSelection(ExplorerState& s, int left, int top, int contentH, int W) {
    const Rectangle drag=normalizedRect(s.selectionDragStart,s.selectionDragCurrent);
    s.selection=s.selectionDragAdditive?s.selectionDragBase:std::set<int>{};
    const int headerH=34;
    if(s.view==ViewMode::Details || s.view==ViewMode::List){
        const int rowH=s.view==ViewMode::Details?34:30;
        const int visible=std::max(1,(contentH-headerH)/rowH);
        for(int i=0;i<visible && s.scroll+i<(int)s.visibleIndices.size();++i){
            const int idx=s.visibleIndices[s.scroll+i];
            Rectangle rr{(float)left,(float)(top+headerH+i*rowH),(float)(W-left-1),(float)rowH};
            if(rectsOverlap(drag,rr)) s.selection.insert(idx);
        }
    }else{
        const int cellW=s.view==ViewMode::MediumIcons?135:180, cellH=s.view==ViewMode::MediumIcons?110:150;
        const int cols=std::max(1,(W-left-10)/cellW), rowsVisible=std::max(1,(contentH-34)/cellH);
        for(int r=0;r<rowsVisible;++r) for(int c=0;c<cols;++c){
            const int pos=(s.scroll+r)*cols+c; if(pos>=(int)s.visibleIndices.size()) break;
            const int idx=s.visibleIndices[pos];
            Rectangle rr{(float)(left+7+c*cellW),(float)(top+34+r*cellH),(float)cellW-7,(float)cellH-7};
            if(rectsOverlap(drag,rr)) s.selection.insert(idx);
        }
    }
    if(!s.selection.empty()) s.selected=*s.selection.rbegin();
}

static bool isElfFile(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return false;
    std::ifstream in(p, std::ios::binary);
    unsigned char magic[4]{};
    in.read(reinterpret_cast<char*>(magic), 4);
    return in.gcount() == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

static bool executeElfDetached(const fs::path& p) {
    if (p.empty() || access(p.c_str(), X_OK) != 0) return false;
    pid_t leader = fork();
    if (leader < 0) return false;
    if (leader == 0) {
        if (setsid() < 0) _exit(126);
        pid_t child = fork();
        if (child < 0) _exit(126);
        if (child > 0) _exit(0);
        execl(p.c_str(), p.c_str(), (char*)nullptr);
        _exit(127);
    }
    int status = 0;
    (void)waitpid(leader, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void openExternal(const fs::path& p) {
    if (p.empty()) return;
    if (isElfFile(p) && executeElfDetached(p)) return;

    // xdg-open is asynchronous from the user's point of view, but leaving
    // the child unreaped creates a zombie for every opened file. Use the
    // same short-lived-session-leader pattern as terminal/editor launching.
    pid_t leader = fork();
    if (leader < 0) return;
    if (leader == 0) {
        if (setsid() < 0) _exit(126);
        pid_t child = fork();
        if (child < 0) _exit(126);
        if (child > 0) _exit(0);
        execlp("xdg-open", "xdg-open", p.c_str(), (char*)nullptr);
        _exit(127);
    }
    int status = 0;
    (void)waitpid(leader, &status, 0);
}

static bool hasSuffix(const std::string& value, const char* suffix) {
    const size_t len = std::strlen(suffix);
    return value.size() >= len && value.compare(value.size() - len, len, suffix) == 0;
}

static bool isSingleCompressionFile(const fs::path& p) {
    const std::string n = lowerCopy(p.filename().string());
    return (n.size() > 3 && hasSuffix(n, ".gz") && !hasSuffix(n, ".tar.gz") && !hasSuffix(n, ".tgz")) ||
           (n.size() > 4 && hasSuffix(n, ".zst") && !hasSuffix(n, ".tar.zst") && !hasSuffix(n, ".tzst")) ||
           (n.size() > 3 && hasSuffix(n, ".xz") && !hasSuffix(n, ".tar.xz") && !hasSuffix(n, ".txz"));
}

static fs::path singleCompressionOutputPath(const fs::path& p) {
    std::string n = p.filename().string();
    const std::string low = lowerCopy(n);
    auto strip = [&](const char* suffix) {
        const size_t len = std::strlen(suffix);
        if (low.size() >= len && low.compare(low.size()-len, len, suffix) == 0) n.resize(n.size()-len);
    };
    if (hasSuffix(low, ".zst")) strip(".zst");
    else if (hasSuffix(low, ".xz")) strip(".xz");
    else if (hasSuffix(low, ".gz")) strip(".gz");
    if (n.empty()) n = "extracted";
    return p.parent_path() / n;
}

static bool extractSingleCompressionCli(const fs::path& input, const fs::path& output, std::string& error) {
    const std::string low = lowerCopy(input.filename().string());
    const bool isGzip = hasSuffix(low, ".gz");
    const bool isZstd = hasSuffix(low, ".zst");
    const bool isXz = hasSuffix(low, ".xz");
    const char* tool = isGzip ? "gzip" : (isZstd ? "zstd" : (isXz ? "xz" : nullptr));
    if (!tool) { error = "Unsupported single-file compression"; return false; }
    if (!commandExists(tool)) { error = std::string("Cannot find ") + tool + " for " + input.filename().string(); return false; }
    if (fs::exists(output)) {
        error = "Output already exists: " + output.filename().string();
        return false;
    }
    std::ostringstream cmd;
    if (isGzip) {
        cmd << shellQuote(tool) << " -dc -- " << shellQuote(input.string()) << " > " << shellQuote(output.string());
    } else {
        cmd << shellQuote(tool) << " -d -c -- " << shellQuote(input.string()) << " > " << shellQuote(output.string());
    }
    pid_t pid = fork();
    if (pid < 0) { error = std::strerror(errno); return false; }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-lc", cmd.str().c_str(), (char*)nullptr);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { error = std::strerror(errno); return false; }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    std::error_code ec;
    fs::remove(output, ec);
    error = std::string("Can't extract ") + input.filename().string() + ": " + tool + " failed";
    return false;
}

static void extractSingleCompressionFile(ExplorerState& s, const fs::path& p, bool openResult) {
    if (!s.vfs || !s.vfs->isLocal()) { s.status = "Single-file compression requires a local filesystem"; return; }
    const fs::path out = singleCompressionOutputPath(p);
    std::string error;
    if (!extractSingleCompressionCli(p, out, error)) { s.status = error; return; }
    s.status = "Extracted: " + out.filename().string();
    refresh(s);
    if (openResult) openExternal(out);
}

static void openSelected(ExplorerState& s) {
    const auto rows=selectedRows(s); if (rows.size()!=1) return;
    const auto e=s.rows[rows.front()];
    if (e.kind == EntryKind::Directory) navigate(s,e.path,true);
    else if (e.kind == EntryKind::Archive && isSingleCompressionFile(e.path)) extractSingleCompressionFile(s,e.path,true);
    else if (e.kind == EntryKind::Archive) openArchive(s,e.path);
    else { openExternal(e.path); s.status = "Opening " + e.name; }
}

static std::string typeLabel(const VfsEntry& e) {
    if (e.kind == EntryKind::Directory) return "File folder";
    if (e.kind == EntryKind::Archive) return "Archive";
    return "File";
}

static rayicons::Icon iconForEntry(const VfsEntry& e) {
    if (e.kind == EntryKind::Directory) return rayicons::FolderOpen;
    if (e.kind == EntryKind::Archive) return rayicons::FileCopy;
    const std::string n=lowerCopy(e.name); const auto dot=n.find_last_of('.'); const std::string ext=dot==std::string::npos?"":n.substr(dot);
    if (ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".gif"||ext==".bmp"||ext==".webp") return rayicons::FileImage;
    if (ext==".mp3"||ext==".wav"||ext==".ogg"||ext==".flac") return rayicons::FileAudio;
    if (ext==".mp4"||ext==".mkv"||ext==".avi"||ext==".webm") return rayicons::FileVideo;
    if (ext==".txt"||ext==".md"||ext==".cpp"||ext==".c"||ext==".h"||ext==".hpp"||ext==".py"||ext==".rs") return rayicons::FileText;
    return rayicons::FileOpen;
}

static void drawInlineEditor(Rectangle r, const std::string& v, const TextEditor& ed, const Theme& t, int fs) {
    const int fontPx = uiFont(fs);
    if (!ed.text) {
        BeginScissorMode((int)r.x+4,(int)r.y+2,std::max(1,(int)r.width-8),std::max(1,(int)r.height-4));
        DrawText(v.c_str(),(int)r.x+10,(int)r.y+8,fontPx,t.text);
        EndScissorMode();
        return;
    }
    const int baseX=(int)r.x+10, baseY=(int)r.y+((int)r.height>=38?10:8);
    const int innerW=std::max(1,(int)r.width-16);
    const auto measurePrefix = [&](size_t n){ return MeasureText(v.substr(0,n).c_str(),fontPx); };
    int anchorPixel=measurePrefix(ed.cursor);
    int scrollX=std::max(0, anchorPixel-innerW);
    if (anchorPixel-scrollX < 0) scrollX=anchorPixel;

    BeginScissorMode((int)r.x+4,(int)r.y+2,std::max(1,(int)r.width-8),std::max(1,(int)r.height-4));
    const int drawX=baseX-scrollX;
    if (ed.hasSelection()) {
        const int a=measurePrefix(ed.lo())-scrollX;
        const int b=measurePrefix(ed.hi())-scrollX;
        DrawRectangle(drawX+a,baseY+1,std::max(1,b-a),fs+2,Fade(t.accent,0.30f));
    }
    DrawText(v.c_str(),drawX,baseY,fontPx,t.text);
    if (ed.cursor<=v.size()) {
        const int tw=measurePrefix(ed.cursor)-scrollX;
        DrawLine(baseX+tw,(int)r.y+6,baseX+tw,(int)r.y+r.height-6,t.accent);
    }
    EndScissorMode();
}


static bool isImagePath(const std::string& name) {
    const std::string n=lowerCopy(name);
    const auto d=n.find_last_of('.');
    if(d==std::string::npos) return false;
    const std::string e=n.substr(d);
    return e==".png"||e==".jpg"||e==".jpeg"||e==".webp"||e==".gif"||e==".bmp"||
           e==".tif"||e==".tiff"||e==".avif"||e==".jxl"||e==".heic"||e==".heif"||
           e==".jp2"||e==".j2k"||e==".tga"||e==".exr"||e==".fits";
}

struct ThumbnailJob {
    std::string key;
    std::string path;
    int maxSize = 128;
};

struct ThumbnailResult {
    std::string key;
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
    bool ok = false;
};

class ThumbnailWorker {
public:
    void start(int workerCount) {
        stopRequested = false;
        workerCount = std::clamp(workerCount, 1, 2);
        for (int i = 0; i < workerCount; ++i)
            workers.emplace_back(&ThumbnailWorker::run, this);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopRequested = true;
        }
        cv.notify_all();
        for (auto &t : workers) if (t.joinable()) t.join();
        workers.clear();
        std::lock_guard<std::mutex> lock(mutex);
        jobs.clear();
        pending.clear();
    }

    bool enqueue(const std::string& key, const std::string& path, int maxSize) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopRequested || pending.count(key)) return false;
        pending.insert(key);
        jobs.push_back({key, path, maxSize});
        cv.notify_one();
        return true;
    }

    std::vector<ThumbnailResult> drain(size_t maxCount) {
        std::vector<ThumbnailResult> out;
        std::lock_guard<std::mutex> lock(mutex);
        while (!results.empty() && out.size() < maxCount) {
            pending.erase(results.front().key);
            out.push_back(std::move(results.front()));
            results.pop_front();
        }
        return out;
    }

    bool hasPendingWork() const {
        std::lock_guard<std::mutex> lock(mutex);
        return !jobs.empty() || !pending.empty();
    }

private:
    void run() {
        for (;;) {
            ThumbnailJob job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&]{ return stopRequested || !jobs.empty(); });
                if (stopRequested && jobs.empty()) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }

            ThumbnailResult result;
            result.key = job.key;

            VipsImage* img = nullptr;
            if (vips_thumbnail(job.path.c_str(), &img, job.maxSize, nullptr) == 0 && img) {
                VipsImage* rgba = nullptr;
                const int bands = vips_image_get_bands(img);
                if (bands == 4 || bands == 3) rgba = (VipsImage*)g_object_ref(img);
                if (rgba && vips_image_get_bands(rgba) == 3) {
                    VipsImage* alpha = nullptr;
                    if (vips_addalpha(rgba, &alpha, nullptr) == 0) {
                        g_object_unref(rgba);
                        rgba = alpha;
                    }
                }
                if (rgba && vips_image_get_bands(rgba) == 4) {
                    size_t bytes = 0;
                    void* mem = vips_image_write_to_memory(rgba, &bytes);
                    if (mem && bytes > 0) {
                        result.width = vips_image_get_width(rgba);
                        result.height = vips_image_get_height(rgba);
                        result.pixels.assign((unsigned char*)mem, (unsigned char*)mem + bytes);
                        result.ok = !result.pixels.empty();
                        g_free(mem);
                    }
                }
                if (rgba) g_object_unref(rgba);
                g_object_unref(img);
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                // Keep the key in pending until the UI drains this result.
                // This prevents duplicate jobs while a completed thumbnail is
                // waiting for the render thread to upload its texture.
                results.push_back(std::move(result));
            }
        }
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<ThumbnailJob> jobs;
    std::deque<ThumbnailResult> results;
    std::unordered_set<std::string> pending;
    std::vector<std::thread> workers;
    bool stopRequested = false;
};

static ThumbnailWorker* gThumbnailWorker = nullptr;

static size_t thumbnailCacheLimit(const ExplorerState& s) {
    if (s.view == ViewMode::MediumIcons) return 96;
    if (s.view == ViewMode::LargeIcons) return 48;
    return 16;
}

static void evictOldestThumbnail(ExplorerState& s) {
    const size_t limit = thumbnailCacheLimit(s);
    while (s.thumbnailCache.size() >= limit) {
        auto victim = s.thumbnailUse.end();
        for (auto it = s.thumbnailUse.begin(); it != s.thumbnailUse.end(); ++it) {
            if (s.thumbnailProtected.count(it->first)) continue;
            if (victim == s.thumbnailUse.end() || it->second < victim->second) victim = it;
        }
        if (victim == s.thumbnailUse.end()) return;
        auto tex = s.thumbnailCache.find(victim->first);
        if (tex != s.thumbnailCache.end() && tex->second.id) UnloadTexture(tex->second);
        s.thumbnailCache.erase(victim->first);
        s.thumbnailUse.erase(victim);
    }
}

static void pumpThumbnailResults(ExplorerState& s) {
    if (!gThumbnailWorker) return;
    // Upload only a few textures per frame. CPU-side libvips decoding stays off-thread,
    // while bounded GPU uploads prevent a large directory from causing frame spikes.
    for (ThumbnailResult result : gThumbnailWorker->drain(3)) {
        if (!result.ok || result.width <= 0 || result.height <= 0 || result.pixels.empty()) {
            s.thumbnailFailed.insert(result.key);
            continue;
        }
        s.thumbnailFailed.erase(result.key);
        if (s.thumbnailCache.find(result.key) != s.thumbnailCache.end()) continue;
        evictOldestThumbnail(s);
        Image image{};
        image.data = result.pixels.data();
        image.width = result.width;
        image.height = result.height;
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D tex = LoadTextureFromImage(image);
        if (!tex.id) continue;
        s.thumbnailCache.emplace(result.key, tex);
        s.thumbnailUse[result.key] = ++s.thumbnailUseCounter;
    }
}

static bool vipsToTexture(VipsImage* img, Texture2D& out, int& w, int& h) {
    if(!img) return false;
    VipsImage* rgba=nullptr;
    int bands=vips_image_get_bands(img);
    if(bands==4) rgba=(VipsImage*)g_object_ref(img);
    else if(bands==3) rgba=(VipsImage*)g_object_ref(img);
    if(!rgba) return false;
    VipsImage* alpha=nullptr;
    if(vips_image_get_bands(rgba)==3) { if(vips_addalpha(rgba,&alpha,nullptr)==0) { g_object_unref(rgba); rgba=alpha; } }
    size_t memSize=0; void* mem=vips_image_write_to_memory(rgba,&memSize);
    if(!mem){ g_object_unref(rgba); return false; }
    w=vips_image_get_width(rgba); h=vips_image_get_height(rgba);
    Image im{}; im.data=mem; im.width=w; im.height=h; im.mipmaps=1; im.format=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    if(out.id) UnloadTexture(out);
    out=LoadTextureFromImage(im);
    g_free(mem); g_object_unref(rgba); return out.id!=0;
}

static bool loadVipsImageTexture(const std::string& path, int maxSize, Texture2D& out, int& w, int& h) {
    VipsImage* img=nullptr;
    if(vips_thumbnail(path.c_str(),&img,maxSize,nullptr)!=0) return false;
    bool ok=vipsToTexture(img,out,w,h); g_object_unref(img); return ok;
}

static int configuredThumbnailSize(const Config& c) {
    static const int sizes[] = {32, 64, 128, 256, 512};
    return sizes[std::clamp(c.thumbnailRes,1,5)-1];
}

static bool ensureThumbnail(ExplorerState& s, const VfsEntry& e, int maxSize, Texture2D& tex) {
    // Never build thumbnails for ArchiveVfs: image thumbnails are a local-filesystem feature.
    if(!s.config.showThumbnails || e.kind!=EntryKind::File || !isImagePath(e.name) || !s.vfs || !s.vfs->isLocal()) return false;
    const int target = maxSize > 0 ? maxSize : configuredThumbnailSize(s.config);
    const std::string key = e.path + "#" + std::to_string(target);
    if (s.thumbnailFailed.count(key)) return false;
    auto it=s.thumbnailCache.find(key);
    if(it!=s.thumbnailCache.end()) {
        s.thumbnailUse[key] = ++s.thumbnailUseCounter;
        tex=it->second;
        return tex.id!=0;
    }
    if(gThumbnailWorker) gThumbnailWorker->enqueue(key, e.path, target);
    return false;
}

static void openImageViewer(ExplorerState& s) {
    const auto ids=selectedRows(s); if(ids.size()!=1 || !s.vfs || !s.vfs->isLocal()) return;
    const VfsEntry& e=s.rows[ids.front()]; if(!isImagePath(e.name)){s.status="Selected file is not an image";return;}
    if (s.imageTexture.id) {
        UnloadTexture(s.imageTexture);
        s.imageTexture = {};
    }
    s.imageW = s.imageH = 0;
    s.imagePath = e.path;
    s.imageZoom = 1.0f;
    s.flags.imageFit = true;
    s.imagePan = {0, 0};
    s.flags.imageDragging = false;
    if(!loadVipsImageTexture(e.path,4096,s.imageTexture,s.imageW,s.imageH)){s.status="Cannot load image";return;}
    s.modal=Modal::ImageView; s.menu.open=false; unfocus(s);
}

static Color entryIconColor(const VfsEntry& e, const Theme& t) {
    // Paper White uses black glyphs; all other themes follow the configured accent.
    return (t.bg.r > 240 && t.bg.g > 240 && t.bg.b > 240) ? t.text : t.accent;
}

static void drawEntryIcon(const VfsEntry& e, int x, int y, int targetSize, Color c) {
    rayicons::Draw(iconForEntry(e),x,y,std::max(1,targetSize/16),c);
}

static bool pointIn(Rectangle r, Vector2 p) { return CheckCollisionPointRec(p,r); }

static void pathInfo(const fs::path& p, const MagicDatabase& magic, std::string& desc, std::string& mime, mode_t& mode) {
    struct stat st{};
    if (::lstat(p.c_str(), &st) == 0) mode = st.st_mode & 07777;
    const MagicInfo mi=magic.file(p); desc=mi.description; mime=mi.mime;
}

static std::string octal(mode_t m) {
    char buf[16]; std::snprintf(buf,sizeof(buf),"%04o",(unsigned)(m & 07777)); return buf;
}

static mode_t parseOctal(const std::string& s, mode_t fallback) {
    if (s.empty() || s.size() > 4) return fallback;
    unsigned v=0;
    for (char c:s) { if (c<'0'||c>'7') return fallback; v=(v<<3)+(unsigned)(c-'0'); }
    return (mode_t)(v & 07777);
}

static void modeToBits(PropertiesState& p) { p.mode=parseOctal(p.modeEdit,p.mode); }
static void bitsToModeEdit(PropertiesState& p) { p.modeEdit=octal(p.mode); }

static void toggleModeBit(PropertiesState& p, int row, int col) {
    static const mode_t bits[3][3] = {{S_IRUSR,S_IWUSR,S_IXUSR},{S_IRGRP,S_IWGRP,S_IXGRP},{S_IROTH,S_IWOTH,S_IXOTH}};
    if (row<0||row>=3||col<0||col>=3) return;
    p.mode ^= bits[row][col]; bitsToModeEdit(p);
}

static bool copyRecursive(const fs::path& src, const fs::path& dst, std::string& error) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(src, ec);
    if (ec) { error = ec.message(); return false; }
    if (src == dst || (fs::is_directory(st) && dst.string().rfind(src.string()+"/",0)==0)) {
        error = "Cannot copy a directory into itself";
        return false;
    }
    if (fs::is_symlink(st)) {
        std::error_code rmec;
        fs::remove(dst, rmec);
        const fs::path target = fs::read_symlink(src, ec);
        if (ec) { error = ec.message(); return false; }
        fs::create_symlink(target, dst, ec);
        if (ec) { error = ec.message(); return false; }
        return true;
    }
    if (fs::is_directory(st)) {
        fs::create_directories(dst, ec);
        if (ec) { error = ec.message(); return false; }
        fs::directory_iterator it(src, fs::directory_options::skip_permission_denied, ec), endIt;
        if (ec) { error = ec.message(); return false; }
        for (; it != endIt; it.increment(ec)) {
            if (ec) { error = ec.message(); return false; }
            const fs::directory_entry child = *it;
            if (!copyRecursive(child.path(), dst / child.path().filename(), error)) return false;
        }
        return true;
    }
    if (fs::is_regular_file(st)) {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) { error = ec.message(); return false; }
        return true;
    }
    error = "Unsupported file type: " + src.string();
    return false;
}

static bool removeRecursive(const fs::path& p, std::string& error) {
    std::error_code ec; fs::remove_all(p,ec); if (ec) { error=ec.message(); return false; } return true;
}

static void doCopyOrCut(ExplorerState& s, bool cut) {
    s.clipboard.paths.clear(); s.clipboard.cut=cut;
    if (!s.vfs || !s.vfs->isLocal()) { s.status="Clipboard operations require a local folder"; return; }
    for (int idx : selectedRows(s)) if (idx>=0 && idx<(int)s.rows.size()) s.clipboard.paths.push_back(s.rows[idx].path);
    s.status = cut ? "Cut " + std::to_string(s.clipboard.paths.size()) + " item(s)" : "Copied " + std::to_string(s.clipboard.paths.size()) + " item(s)";
}

static bool finishPasteOne(ExplorerState& s, const fs::path& src, const fs::path& dst, bool cut, bool overwrite) {
    std::string error;
    if (cut) {
        std::error_code ec;
        if (overwrite && fs::exists(dst, ec)) {
            fs::remove_all(dst, ec);
            if (ec) { s.status = "Paste failed: " + ec.message(); return false; }
        }
        fs::rename(src, dst, ec);
        if (ec) {
            if (!copyRecursive(src, dst, error) || !removeRecursive(src, error)) {
                s.status = "Paste failed: " + error;
                return false;
            }
        }
    } else {
        if (overwrite) {
            std::error_code ec;
            if (fs::exists(dst, ec)) {
                fs::remove_all(dst, ec);
                if (ec) { s.status = "Paste failed: " + ec.message(); return false; }
            }
        }
        if (!copyRecursive(src, dst, error)) {
            s.status = "Paste failed: " + error;
            return false;
        }
    }
    return true;
}

static void continuePasteClipboard(ExplorerState& s) {
    if (!s.vfs || !s.vfs->isLocal() || s.clipboard.paths.empty()) return;
    const fs::path destDir = s.path;

    while (s.pendingPasteIndex < s.clipboard.paths.size()) {
        const fs::path src = s.clipboard.paths[s.pendingPasteIndex];
        const fs::path dst = destDir / src.filename();
        std::error_code ec;
        if (fs::exists(dst, ec)) {
            s.pendingPasteSource = src;
            s.pendingPasteDestination = dst;
            s.flags.pendingPasteCut = s.clipboard.cut;
            s.modal = Modal::PasteOverwrite;
            s.menu.open = false;
            unfocus(s);
            return;
        }
        if (!finishPasteOne(s, src, dst, s.clipboard.cut, false)) {
            s.clipboard.paths.clear();
            s.clipboard.cut = false;
            s.pendingPasteIndex = 0;
            refresh(s);
            return;
        }
        ++s.pendingPasteIndex;
    }

    s.clipboard.paths.clear();
    s.clipboard.cut = false;
    s.pendingPasteIndex = 0;
    refresh(s);
    s.status = "Paste complete";
}

static void pasteClipboard(ExplorerState& s) {
    if (!s.vfs || !s.vfs->isLocal() || s.clipboard.paths.empty()) return;
    s.pendingPasteIndex = 0;
    continuePasteClipboard(s);
}


static bool runCommandPath(const char* cmd, const fs::path& p) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) { execlp(cmd, cmd, p.c_str(), (char*)nullptr); _exit(127); }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool xdgTrash(const fs::path& p) {
    pid_t pid=fork();
    if(pid<0) return false;
    if(pid==0){ execlp("gio","gio","trash",p.c_str(),(char*)nullptr); _exit(127); }
    int status=0; if(waitpid(pid,&status,0)<0) return false;
    if(WIFEXITED(status) && WEXITSTATUS(status)==0) return true;
    return runCommandPath("trash-put",p);
}

static void openPermanentDeleteConfirm(ExplorerState& s) {
    if (!s.vfs || !s.vfs->isLocal()) return;
    s.pendingPermanentDelete.clear();
    for (int idx : selectedRows(s)) {
        if (idx >= 0 && idx < (int)s.rows.size()) s.pendingPermanentDelete.emplace_back(s.rows[idx].path);
    }
    if (s.pendingPermanentDelete.empty()) return;
    s.modal = Modal::ConfirmPermanentDelete;
    s.modalError.clear();
    s.menu.open = false;
    unfocus(s);
}

static void applyPermanentDelete(ExplorerState& s) {
    if (!s.vfs || !s.vfs->isLocal() || s.pendingPermanentDelete.empty()) return;
    std::string error;
    for (const fs::path& p : s.pendingPermanentDelete) {
        if (!removeRecursive(p, error)) {
            s.status = "Permanent delete failed: " + error;
            s.modal = Modal::None;
            s.pendingPermanentDelete.clear();
            return;
        }
    }
    const size_t count = s.pendingPermanentDelete.size();
    s.pendingPermanentDelete.clear();
    s.modal = Modal::None;
    refresh(s);
    s.status = "Permanently deleted " + std::to_string(count) + (count == 1 ? " item" : " items");
}

static void deleteSelection(ExplorerState& s, bool permanent=false) {
    if (!s.vfs || !s.vfs->isLocal()) return;
    std::string error;
    for (int idx : selectedRows(s)) {
        if (idx < 0 || idx >= (int)s.rows.size()) continue;
        const fs::path p=s.rows[idx].path;
        bool ok = permanent ? removeRecursive(p,error) : xdgTrash(p);
        if (!ok) {
            if (!permanent) { error = "No XDG trash tool (gio/trash-put) or trash failed"; }
            s.status=std::string(permanent?"Delete failed: ":"Trash failed: ")+error; return;
        }
    }
    refresh(s); s.status=permanent?"Permanently deleted":"Moved to Trash";
}

static void openCat(ExplorerState& s) {
    const auto ids=selectedRows(s);
    if (ids.size()!=1 || !s.vfs || !s.vfs->isLocal()) { s.status="cat needs one local file"; return; }
    const fs::path p=s.rows[ids.front()].path;
    std::error_code ec;
    if (!fs::is_regular_file(p,ec)) { s.status="cat: not a regular file"; return; }
    const auto sz=fs::file_size(p,ec);
    if (ec || sz > 8*1024*1024) { s.status="cat: file too large (max 8 MiB)"; return; }
    std::ifstream in(p, std::ios::binary);
    if (!in) { s.status="cat: cannot open file"; return; }
    s.catText.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    s.catScroll=0; s.modal=Modal::Cat; unfocus(s); s.menu.open=false;
}


static std::uintmax_t directorySizeRecursive(const fs::path& root, std::uint64_t& itemCount) {
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) { ++itemCount; std::error_code fec; return fs::file_size(root, fec); }
    if (!fs::is_directory(root, ec)) return 0;
    std::uintmax_t total=0;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for(;it!=end;it.increment(ec)){
        if(ec){ec.clear();continue;}
        const auto& e=*it; std::error_code sec;
        if(e.is_regular_file(sec)){++itemCount;std::error_code fec;auto n=e.file_size(fec);if(!fec)total+=n;}
        else if(e.is_directory(sec)){++itemCount;}
    }
    return total;
}

static std::string unescapeMountField(const std::string& s){
    std::string out; out.reserve(s.size());
    for(size_t i=0;i<s.size();++i){
        if(s[i]=='\\'&&i+3<s.size()&&s[i+1]=='0'&&s[i+2]=='4'&&s[i+3]=='0'){out.push_back(' ');i+=3;}
        else if(s[i]=='\\'&&i+3<s.size()&&s[i+1]=='0'&&s[i+2]=='1'&&s[i+3]=='1'){out.push_back('\t');i+=3;}
        else if(s[i]=='\\'&&i+3<s.size()&&s[i+1]=='0'&&s[i+2]=='1'&&s[i+3]=='2'){out.push_back('\n');i+=3;}
        else if(s[i]=='\\'&&i+3<s.size()&&s[i+1]=='0'&&s[i+2]=='1'&&s[i+3]=='4'){out.push_back('\\');i+=3;}
        else out.push_back(s[i]);
    }
    return out;
}

static std::vector<DiskMountInfo> collectDiskMounts(){
    std::vector<DiskMountInfo> out; std::ifstream in("/proc/self/mounts"); std::string src,mnt,fstype,opts,a,b; std::set<std::string> seen;
    while(in>>src>>mnt>>fstype>>opts>>a>>b){
        src=unescapeMountField(src); mnt=unescapeMountField(mnt);
        if(mnt.empty()||mnt[0]!='/'||seen.count(mnt))continue; struct statvfs st{}; if(::statvfs(mnt.c_str(),&st)!=0)continue;
        std::uint64_t total=std::uint64_t(st.f_blocks)*std::uint64_t(st.f_frsize), free=std::uint64_t(st.f_bavail)*std::uint64_t(st.f_frsize), used=total>free?total-free:0;
        bool root=mnt=="/", dev=src.rfind("/dev/",0)==0||src.rfind("UUID=",0)==0||src.rfind("LABEL=",0)==0;
        bool pseudo=fstype=="proc"||fstype=="sysfs"||fstype=="devpts"||fstype=="cgroup"||fstype=="cgroup2"||fstype=="mqueue"||fstype=="pstore"||fstype=="debugfs"||fstype=="tracefs"||fstype=="securityfs";
        if(!root&&!dev&&pseudo)continue; seen.insert(mnt); out.push_back({src,fs::path(mnt),fstype,total,used,free});
    }
    std::sort(out.begin(),out.end(),[](const auto&a,const auto&b){if(a.mountpoint=="/")return true;if(b.mountpoint=="/")return false;return a.mountpoint.string()<b.mountpoint.string();});
    return out;
}
static std::string diskPercent(const DiskMountInfo& d){ if(!d.total)return "0.0%"; char b[32]; std::snprintf(b,sizeof(b),"%.1f%%",100.0*(double)d.used/(double)d.total); return b; }
static std::string mountLabel(const DiskMountInfo& d){ if(d.mountpoint=="/")return "Root (/)"; std::string s=d.mountpoint.string(); if(s.size()>20)s=s.substr(0,17)+"..."; return s; }
static void openDiskInfo(ExplorerState& s){ s.diskInfo.mounts=collectDiskMounts(); if(s.diskInfo.mounts.empty()){s.status="No mounted filesystems available";return;} s.diskInfo.tab=0;s.diskInfo.firstTab=0;s.modal=Modal::DiskInfo;s.menu.open=false;unfocus(s); }

static void startPropertiesForPath(ExplorerState& s, const fs::path& p) {
    if (!s.vfs || !s.vfs->isLocal() || p.empty()) { s.status="Properties are available for local files and directories"; return; }
    std::error_code ec;
    if (!fs::exists(p, ec)) { s.status="Path no longer exists"; return; }
    s.props.path=p.string();
    s.props.multi=false;
    s.props.selectedCount=1;
    pathInfo(p.string(),s.magic,s.props.magic.description,s.props.magic.mime,s.props.mode);
    bitsToModeEdit(s.props);
    s.props.itemCount=0;
    s.props.totalSize=directorySizeRecursive(p,s.props.itemCount);
    const auto ft = fs::last_write_time(p, ec);
    s.props.mtime = ec ? 0 : fileTimeToTimeT(ft);
    s.modal=Modal::Properties; s.modalError.clear(); unfocus(s); s.menu.open=false;
}

static void startProperties(ExplorerState& s) {
    const auto ids=selectedRows(s);
    if (ids.empty() || !s.vfs || !s.vfs->isLocal()) { s.status="Properties are available for local items"; return; }
    if (ids.size() == 1) {
        startPropertiesForPath(s, fs::path(s.rows[ids.front()].path));
        return;
    }
    s.props = PropertiesState{};
    s.props.multi = true;
    s.props.selectedCount = ids.size();
    s.props.path = std::to_string(ids.size()) + " selected items";
    for (int idx : ids) {
        if (idx < 0 || idx >= (int)s.rows.size()) continue;
        const fs::path p = fs::path(s.rows[idx].path);
        std::uint64_t count = 0;
        s.props.totalSize += directorySizeRecursive(p, count);
        s.props.itemCount += count ? count : 1;
    }
    s.modal = Modal::Properties;
    s.modalError.clear();
    unfocus(s);
    s.menu.open = false;
}

static void startCurrentDirectoryProperties(ExplorerState& s) {
    if(!s.vfs || !s.vfs->isLocal() || s.path.empty()) { s.status="Properties require a local directory"; return; }
    startPropertiesForPath(s, fs::path(s.path));
}

static void startRename(ExplorerState& s) {
    const auto ids=selectedRows(s); if (ids.size()!=1 || !s.vfs || !s.vfs->isLocal()) return;
    s.renameEdit=s.rows[ids.front()].name; s.modal=Modal::Rename; s.modalError.clear(); focus(s,TextField::Rename,true); s.menu.open=false;
}

static void applyRename(ExplorerState& s) {
    if (s.selected<0 || !s.vfs || !s.vfs->isLocal() || s.renameEdit.empty()) return;
    const fs::path src=s.rows[s.selected].path; const fs::path dst=src.parent_path()/s.renameEdit;
    std::error_code ec; fs::rename(src,dst,ec);
    if (ec) { s.modalError=ec.message(); return; }
    s.modal=Modal::None; unfocus(s); refresh(s); s.status="Renamed";
}

static void saveProperties(ExplorerState& s) {
    if (s.props.multi) { s.modal=Modal::None; unfocus(s); return; }
    modeToBits(s.props);
    if (s.props.path.empty()) return;
    if (::chmod(s.props.path.c_str(), s.props.mode & 07777) != 0) { s.modalError=std::strerror(errno); return; }
    s.modal=Modal::None; unfocus(s); refresh(s); s.status="Permissions updated to "+octal(s.props.mode);
}

static std::string shellQuote(const std::string& in) {
    std::string out="'";
    for(char c:in){ if(c=='\'') out += "'\"'\"'"; else out += c; }
    out += "'";
    return out;
}

static bool spawnShellInDir(const fs::path& dir, const std::string& command, bool inheritIO=true) {
    pid_t leader=fork();
    if(leader<0) return false;
    if(leader==0){
        if(setsid()<0) _exit(126);
        pid_t child=fork();
        if(child<0) _exit(126);
        if(child>0) _exit(0);
        if(chdir(dir.c_str())!=0) _exit(126);
        if(!inheritIO){ int devnull=open("/dev/null",O_RDWR); if(devnull>=0){dup2(devnull,0);dup2(devnull,1);dup2(devnull,2);close(devnull);} }
        execl("/bin/sh","sh","-lc",command.c_str(),(char*)nullptr); _exit(127);
    }
    int status=0; (void)waitpid(leader,&status,0);
    return true;
}

static bool spawnDetachedShellInDir(const fs::path& dir, const std::string& command) {
    pid_t pid=fork();
    if(pid<0) return false;
    if(pid==0){
        if(setsid()<0) _exit(126);
        pid_t child=fork();
        if(child<0) _exit(126);
        if(child>0) _exit(0);
        if(chdir(dir.c_str())!=0) _exit(126);
        int devnull=open("/dev/null",O_RDWR);
        if(devnull>=0){ dup2(devnull,STDIN_FILENO); dup2(devnull,STDOUT_FILENO); dup2(devnull,STDERR_FILENO); close(devnull); }
        execl("/bin/sh","sh","-lc",command.c_str(),(char*)nullptr);
        _exit(127);
    }
    // Reap only the short-lived session leader; the grandchild is intentionally
    // detached and reparented so the editor/terminal survives independently.
    int status=0; (void)waitpid(pid,&status,0);
    return true;
}

static std::string editorLaunchCommand(const std::string& configured, const fs::path& file) {
    std::istringstream in(configured);
    std::string first;
    in >> first;
    std::string rest;
    std::getline(in,rest);
    if (!rest.empty() && rest.front()==' ') rest.erase(rest.begin());
    const std::string q=shellQuote(file.string());
    if (first.empty()) return {};
    if(first=="alacritty" || first=="kitty" || first=="xterm" || first=="urxvt" || first=="rxvt" || first=="lxterminal" || first=="xfce4-terminal") {
        return first + " -e " + (rest.empty() ? "vi" : rest) + " " + q;
    }
    if(first=="foot") return first + " " + (rest.empty() ? "vi" : rest) + " " + q;
    if(first=="konsole") return first + " --hold -e " + (rest.empty() ? "vi" : rest) + " " + q;
    if(first=="wezterm") return first + " start -- " + (rest.empty() ? "vi" : rest) + " " + q;
    return configured + " " + q;
}

static void openTerminalInDirectory(ExplorerState& s, const fs::path& p) {
    if(!s.vfs || !s.vfs->isLocal()) { s.status="Terminal requires a local directory"; return; }
    std::error_code ec; if(!fs::is_directory(p,ec)) { s.status="Select a directory"; return; }
    if(s.config.term.empty()) { s.status="Set TERM in ~/.config/raymothfm/config"; return; }
    if(spawnDetachedShellInDir(p,s.config.term)) s.status="Opened terminal in: "+p.string();
}

static void openInTerminal(ExplorerState& s) {
    const auto ids=selectedRows(s);
    if(ids.size()!=1 || !s.vfs || !s.vfs->isLocal()) { s.status="Terminal requires one local directory"; return; }
    openTerminalInDirectory(s, fs::path(s.rows[ids.front()].path));
}

static void openCurrentDirectoryInTerminal(ExplorerState& s) {
    if(!s.vfs || !s.vfs->isLocal() || s.path.empty()) { s.status="Terminal requires a local directory"; return; }
    openTerminalInDirectory(s, fs::path(s.path));
}

static void openInEditor(ExplorerState& s) {
    const auto ids=selectedRows(s);
    if(ids.size()!=1 || !s.vfs || !s.vfs->isLocal()) { s.status="Editor requires one local item"; return; }
    if(s.config.editor.empty()) { s.status="Set EDITOR in ~/.config/raymothfm/config"; return; }
    const fs::path p=s.rows[ids.front()].path;
    std::error_code ec; if(!fs::is_regular_file(p,ec)) { s.status="Open in editor expects a regular file"; return; }
    const std::string cmd=editorLaunchCommand(s.config.editor,p);
    if(spawnDetachedShellInDir(fs::path(s.path),cmd)) s.status="Opening in editor: "+p.filename().string();
}


static bool computeChecksumFile(const fs::path& path, const char* algorithm, std::string& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "Unable to open file"; return false; }
    if (std::strcmp(algorithm, "crc32") == 0) {
        uLong crc = ::crc32(0L, Z_NULL, 0);
        std::array<char, 1024 * 1024> buf{};
        while (in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            const std::streamsize n = in.gcount();
            if (n > 0) crc = ::crc32(crc, reinterpret_cast<const Bytef*>(buf.data()), (uInt)n);
        }
        if (in.bad()) { error = "Read error"; return false; }
        char hex[16]; std::snprintf(hex, sizeof(hex), "%08lx", (unsigned long)crc);
        out = hex;
        return true;
    }

    const EVP_MD* md = EVP_get_digestbyname(algorithm);
    if (!md) { error = std::string("Unsupported digest: ") + algorithm; return false; }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { error = "Unable to allocate digest context"; return false; }
    bool ok = EVP_DigestInit_ex(ctx, md, nullptr) == 1;
    std::array<char, 1024 * 1024> buf{};
    while (ok && in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        const std::streamsize n = in.gcount();
        if (n > 0) ok = EVP_DigestUpdate(ctx, buf.data(), (size_t)n) == 1;
    }
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int len = 0;
    if (ok) ok = EVP_DigestFinal_ex(ctx, digest, &len) == 1;
    if (in.bad()) ok = false;
    EVP_MD_CTX_free(ctx);
    if (!ok) { error = "Digest calculation failed"; return false; }
    static const char* h = "0123456789abcdef";
    out.clear(); out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) { out.push_back(h[(digest[i] >> 4) & 0xF]); out.push_back(h[digest[i] & 0xF]); }
    return true;
}

static const char* checksumAlgorithmLabel(int i) {
    switch (i) { case 0: return "MD5"; case 1: return "SHA1"; case 2: return "SHA256"; case 3: return "SHA512"; default: return "CRC32"; }
}

static const char* checksumAlgorithmName(int i) {
    switch (i) { case 0: return "md5"; case 1: return "sha1"; case 2: return "sha256"; case 3: return "sha512"; default: return "crc32"; }
}

static void openChecksum(ExplorerState& s, int row, int algorithm) {
    if (!s.vfs || row < 0 || row >= (int)s.rows.size()) return;
    const auto& e = s.rows[row];
    if (e.kind == EntryKind::Directory || !s.vfs->isLocal()) { s.status = "Checksums are available for local files only"; return; }
    s.checksumPath = e.path;
    s.checksumName = std::string(checksumAlgorithmLabel(algorithm)) + " - " + e.name;
    s.checksumValue = "Calculating...";
    s.modal = Modal::Checksum;
    s.menu.open = false;
    unfocus(s);
    std::string err, value;
    if (computeChecksumFile(e.path, checksumAlgorithmName(algorithm), value, err)) s.checksumValue = value;
    else s.checksumValue = "ERROR: " + err;
}

static int checksumSubmenuIndex(const ContextMenu& m) {
    for (int i = 0; i < (int)m.actions.size(); ++i) if (m.actions[i] == MenuAction::Checksum) return i;
    return -1;
}

static void openContextMenu(ExplorerState& s, int row, Vector2 pos) {
    s.menu.open=true; s.menu.row=row; s.menu.pos=pos; s.menu.actions.clear();
    const bool hasItem=row>=0 && row<(int)s.rows.size();
    if (hasItem) {
        s.menu.actions.push_back(MenuAction::Open);
        if (s.rows[row].kind==EntryKind::Directory) s.menu.actions.push_back(MenuAction::OpenTerminal);
        if (s.rows[row].kind==EntryKind::File) {
            if (isImagePath(s.rows[row].name)) { s.menu.actions.push_back(MenuAction::ViewImage); s.menu.actions.push_back(MenuAction::ConvertImage); }
            else s.menu.actions.push_back(MenuAction::Cat);
        }
        if (s.config.editor.size()) s.menu.actions.push_back(MenuAction::OpenEditor);
        s.menu.actions.push_back(MenuAction::Rename);
        s.menu.actions.push_back(MenuAction::Copy);
        s.menu.actions.push_back(MenuAction::CopyPath);
        s.menu.actions.push_back(MenuAction::Cut);
        s.menu.actions.push_back(MenuAction::Delete);
        s.menu.actions.push_back(MenuAction::PermanentDelete);
        if (s.rows[row].kind==EntryKind::Archive) s.menu.actions.push_back(MenuAction::Extract);
        else s.menu.actions.push_back(MenuAction::Compress);
        if (s.rows[row].kind!=EntryKind::Directory) s.menu.actions.push_back(MenuAction::Checksum);
        s.menu.actions.push_back(MenuAction::Refresh);
        s.menu.actions.push_back(MenuAction::Properties);
    } else {
        if (!s.clipboard.paths.empty()) s.menu.actions.push_back(MenuAction::Paste);
        if (s.vfs && s.vfs->isLocal() && !s.path.empty()) {
            s.menu.actions.push_back(MenuAction::OpenTerminal);
            s.menu.actions.push_back(MenuAction::CopyPath);
            s.menu.actions.push_back(MenuAction::Properties);
        }
        s.menu.actions.push_back(MenuAction::NewDirectory);
        s.menu.actions.push_back(MenuAction::NewFile);
        s.menu.actions.push_back(MenuAction::Refresh);
    }
}

static std::string menuLabel(MenuAction a) {
    switch (a) {
        case MenuAction::Open:return "Open"; case MenuAction::Cat:return "cat / View text"; case MenuAction::ViewImage:return "View image"; case MenuAction::OpenTerminal:return "Open in terminal"; case MenuAction::ConvertImage:return "Convert image..."; case MenuAction::Rename:return "Rename";
        case MenuAction::Copy:return "Copy"; case MenuAction::CopyPath:return "Copy path"; case MenuAction::Cut:return "Cut"; case MenuAction::Paste:return "Paste";
        case MenuAction::Delete:return "Delete (Trash)"; case MenuAction::PermanentDelete:return "Delete permanently";
        case MenuAction::Compress:return "Compress..."; case MenuAction::Extract:return "Extract...";
        case MenuAction::OpenEditor:return "Open in editor"; case MenuAction::Refresh:return "Refresh";
        case MenuAction::NewDirectory:return "New directory..."; case MenuAction::NewFile:return "New file...";
        case MenuAction::Checksum:return "Checksum  >";
        case MenuAction::Properties:return "Properties"; default:return "";
    }
}

static std::string safeMagicOneLine(std::string s) {
    std::replace(s.begin(),s.end(),'\n',' '); if (s.size()>72) s.resize(69),s += "..."; return s;
}

static int visibleRowsFor(ViewMode v,int h){return v==ViewMode::Details?std::max(1,h/34):v==ViewMode::List?std::max(1,h/30):std::max(1,h/(v==ViewMode::MediumIcons?100:145));}

static void drawCheckbox(Rectangle r, bool checked, const char* label, Color text, Color accent) {
    DrawRectangleLinesEx(r,1,checked?accent:text);
    if (checked) { DrawRectangleRec({r.x+3,r.y+3,r.width-6,r.height-6},accent); DrawText("x",(int)r.x+4,(int)r.y+1, uiFont(12), WHITE); }
    DrawText(label,(int)r.x+r.width+7,(int)r.y+1, uiFont(13), text);
}

static bool archivePasswordUIEnabled(const ArchiveOptions& o);
static const char* archiveEncryptionLabel(const ArchiveOptions& o);

static bool convertFormatIs(const ExplorerState& s, std::initializer_list<const char*> names) {
    if (s.convertFormats.empty() || s.convertFormat < 0 || s.convertFormat >= (int)s.convertFormats.size()) return false;
    const std::string& f=s.convertFormats[(size_t)s.convertFormat];
    for (const char* n : names) if (f==n) return true;
    return false;
}

static bool convertQualityEnabled(const ExplorerState& s) {
    if (s.flags.convertLossless) return false;
    return convertFormatIs(s,{"jpg","jpeg","webp","avif","jxl","tif","tiff"});
}
static bool convertEffortEnabled(const ExplorerState& s) {
    return convertFormatIs(s,{"png","webp","avif","jxl"});
}
static bool convertCompressionEnabled(const ExplorerState& s) {
    return convertFormatIs(s,{"png"});
}
static bool convertBitDepthEnabled(const ExplorerState& s) {
    return convertFormatIs(s,{"png","tif","tiff"});
}
static bool convertLosslessEnabled(const ExplorerState& s) {
    return convertFormatIs(s,{"webp","avif","jxl"});
}
static bool convertStripEnabled(const ExplorerState&) { return true; }

static const char* convertFieldValue(const ExplorerState& s,int field) {
    static thread_local std::string tmp;
    switch(field) {
        case 0: return s.convertFormats.empty()?"none":s.convertFormats[s.convertFormat].c_str();
        case 1: tmp=convertQualityEnabled(s)?std::to_string(s.convertQuality):"--"; return tmp.c_str();
        case 2: tmp=convertEffortEnabled(s)?std::to_string(s.convertEffort):"--"; return tmp.c_str();
        case 3: tmp=convertCompressionEnabled(s)?std::to_string(s.convertCompression):"--"; return tmp.c_str();
        case 4: tmp=convertBitDepthEnabled(s)?std::to_string(s.convertBitDepth):"--"; return tmp.c_str();
        case 5: return convertLosslessEnabled(s)?(s.flags.convertLossless?"yes":"no"):"--";
        case 6: return s.flags.convertStrip?"yes":"no";
    }
    return "--";
}

static std::vector<std::string> supportedImageSaveFormats() {
    std::vector<std::string> out;
    gchar** suffixes = vips_foreign_get_suffixes();
    if (suffixes) {
        for (char** it=suffixes; *it; ++it) {
            std::string suf=*it;
            if (suf.empty()) continue;
            if (suf.front()=='.') suf.erase(suf.begin());
            const std::string low=lowerCopy(suf);
            if (low=="jpg"||low=="jpeg"||low=="png"||low=="webp"||low=="avif"||low=="jxl"||low=="tif"||low=="tiff"||low=="gif"||low=="bmp"||low=="heic"||low=="heif"||low=="jp2"||low=="j2k"||low=="ppm"||low=="pgm"||low=="pbm"||low=="pfm"||low=="fits"||low=="exr") out.push_back(low);
        }
        g_strfreev(suffixes);
    }
    std::sort(out.begin(),out.end()); out.erase(std::unique(out.begin(),out.end()),out.end());
    const char* preferred[]={"png","jpg","webp","avif","jxl","tiff","gif","bmp","heic","jp2"};
    std::vector<std::string> ordered;
    for(const char* x:preferred) if(std::find(out.begin(),out.end(),x)!=out.end()) ordered.push_back(x);
    for(const auto& x:out) if(std::find(ordered.begin(),ordered.end(),x)==ordered.end()) ordered.push_back(x);
    return ordered;
}

static bool convertImageFile(ExplorerState& s, std::string& error) {
    if(s.convertInput.empty() || s.convertOutput.empty()) { error="Input/output is empty"; return false; }
    if(s.convertFormats.empty() || s.convertFormat < 0 || s.convertFormat >= (int)s.convertFormats.size()) { error="No output format selected"; return false; }
    std::error_code ec;
    const fs::path input(s.convertInput);
    if(!fs::exists(input,ec) || !fs::is_regular_file(input,ec)) { error="Input image is not a regular file: "+s.convertInput; return false; }
    const char* loader=vips_foreign_find_load(s.convertInput.c_str());
    if(!loader) { error="libvips has no image loader for this file"; return false; }
    vips_error_clear();
    VipsImage* img=vips_image_new_from_file(s.convertInput.c_str(),"access",VIPS_ACCESS_SEQUENTIAL,nullptr);
    if(!img) { const char* e=vips_error_buffer(); error=(e&&*e)?std::string("libvips: ")+e:"libvips could not load the image: "+s.convertInput; vips_error_clear(); return false; }

    const std::string fmt=s.convertFormats[(size_t)s.convertFormat];
    const std::string ext=(fmt=="jpeg"?"jpg":fmt);
    const fs::path outPath(s.convertOutput);
    const std::string tmp=s.convertOutput+".raytmp."+ext;
    std::error_code rmec; fs::remove(tmp,rmec);
    int result=-1;
    if(fmt=="jpg"||fmt=="jpeg") {
        result=vips_image_write_to_file(img,tmp.c_str(),"Q",s.convertQuality,"strip",s.flags.convertStrip,"optimize_coding",true,nullptr);
    } else if(fmt=="webp") {
        if(s.flags.convertLossless) result=vips_image_write_to_file(img,tmp.c_str(),"effort",s.convertEffort,"lossless",true,"strip",s.flags.convertStrip,nullptr);
        else result=vips_image_write_to_file(img,tmp.c_str(),"Q",s.convertQuality,"effort",s.convertEffort,"lossless",false,"strip",s.flags.convertStrip,nullptr);
    } else if(fmt=="avif") {
        if(s.flags.convertLossless) result=vips_image_write_to_file(img,tmp.c_str(),"effort",s.convertEffort,"lossless",true,"strip",s.flags.convertStrip,nullptr);
        else result=vips_image_write_to_file(img,tmp.c_str(),"Q",s.convertQuality,"effort",s.convertEffort,"lossless",false,"strip",s.flags.convertStrip,nullptr);
    } else if(fmt=="jxl") {
        if(s.flags.convertLossless) result=vips_image_write_to_file(img,tmp.c_str(),"effort",s.convertEffort,"lossless",true,"strip",s.flags.convertStrip,nullptr);
        else result=vips_image_write_to_file(img,tmp.c_str(),"Q",s.convertQuality,"effort",s.convertEffort,"lossless",false,"strip",s.flags.convertStrip,nullptr);
    } else if(fmt=="png") {
        result=vips_image_write_to_file(img,tmp.c_str(),"compression",s.convertCompression,"effort",s.convertEffort,"bitdepth",s.convertBitDepth,"strip",s.flags.convertStrip,nullptr);
    } else if(fmt=="tif"||fmt=="tiff") {
        result=vips_image_write_to_file(img,tmp.c_str(),"bitdepth",s.convertBitDepth,"strip",s.flags.convertStrip,nullptr);
    } else {
        result=vips_image_write_to_file(img,tmp.c_str(),"strip",s.flags.convertStrip,nullptr);
    }
    if(result!=0) { const char* e=vips_error_buffer(); error=e&&*e?e:("libvips could not save as "+fmt); vips_error_clear(); g_object_unref(img); fs::remove(tmp,ec); return false; }
    g_object_unref(img);
    fs::rename(tmp,outPath,ec);
    if(ec) { fs::remove(tmp); error=ec.message(); return false; }
    return true;
}

static void openConvertImage(ExplorerState& s) {
    const auto ids=selectedRows(s);
    if(ids.size()!=1 || !s.vfs || !s.vfs->isLocal() || s.rows[ids.front()].kind!=EntryKind::File || !isImagePath(s.rows[ids.front()].name)) { s.status="Select one local image"; return; }
    s.convertInput=s.rows[ids.front()].path;
    s.convertFormats=supportedImageSaveFormats();
    if(s.convertFormats.empty()) { s.status="No libvips image savers are available"; return; }
    std::string ext=lowerCopy(fs::path(s.convertInput).extension().string()); if(!ext.empty()&&ext.front()=='.') ext.erase(ext.begin());
    s.convertFormat=0; for(size_t i=0;i<s.convertFormats.size();++i) if(s.convertFormats[i]==ext){s.convertFormat=(int)i;break;}
    const fs::path in(s.convertInput); s.convertOutput=(in.parent_path()/(in.stem().string()+"_converted."+s.convertFormats[s.convertFormat])).string();
    s.convertQuality=90; s.convertEffort=5; s.convertCompression=6; s.convertBitDepth=8; s.flags.convertLossless=false; s.flags.convertStrip=false; s.flags.convertOutputAuto=true; s.modalError.clear(); s.modalField=0; s.modal=Modal::ConvertImage; s.menu.open=false; focus(s,TextField::ConvertOutput,false);
}

static void cycleConvertField(ExplorerState& s,int field,int delta) {
    switch(field) {
        case 0:
            if(s.convertFormats.empty()) return;
            s.convertFormat=(s.convertFormat+delta+(int)s.convertFormats.size())%(int)s.convertFormats.size();
            if(s.flags.convertOutputAuto) { fs::path in(s.convertInput); const std::string ext=s.convertFormats[s.convertFormat]; s.convertOutput=(in.parent_path()/(in.stem().string()+"_converted."+ext)).string(); }
            break;
        case 1:
            if(!convertQualityEnabled(s)) return;
            { static const int q[]={25,40,55,70,80,90,95,100}; int i=0; while(i<8&&q[i]!=s.convertQuality)++i; if(i>=8)i=5; i=(i+(delta>0?1:-1)+8)%8; s.convertQuality=q[i]; }
            break;
        case 2:
            if(!convertEffortEnabled(s)) return;
            s.convertEffort=std::clamp(s.convertEffort+(delta>0?1:-1),1,10);
            break;
        case 3:
            if(!convertCompressionEnabled(s)) return;
            s.convertCompression=std::clamp(s.convertCompression+(delta>0?1:-1),0,9);
            break;
        case 4:
            if(!convertBitDepthEnabled(s)) return;
            if(convertFormatIs(s,{"tif","tiff"})) { static const int d[]={8,16,32}; int i=0; while(i<3&&d[i]!=s.convertBitDepth)++i; if(i>=3)i=0; i=(i+(delta>0?1:-1)+3)%3; s.convertBitDepth=d[i]; }
            else { s.convertBitDepth=(s.convertBitDepth==8?(delta>0?16:16):(delta>0?8:8)); }
            break;
        case 5:
            if(!convertLosslessEnabled(s)) return;
            s.flags.convertLossless=!s.flags.convertLossless;
            break;
        case 6:
            s.flags.convertStrip=!s.flags.convertStrip;
            break;
    }
}

static void drawModal(ExplorerState& s, int W,int H,const Theme& t) {
    DrawRectangle(0,0,W,H,Fade(BLACK,0.62f));
    const bool largeDialog=(s.modal==Modal::Help);
    Rectangle box{W*0.5f-(largeDialog?350.0f:290.0f),H*0.5f-(largeDialog?280.0f:220.0f),largeDialog?700.0f:580.0f,largeDialog?560.0f:440.0f};
    DrawRectangleRec(box,t.panel2); DrawRectangleLinesEx(box, 1, t.line);
    const std::string title = s.modal==Modal::Properties?"Properties":s.modal==Modal::DiskInfo?"Disk usage":s.modal==Modal::Rename?"Rename":s.modal==Modal::NewItem?(s.flags.newItemDirectory?"New directory":"New file"):s.modal==Modal::CreateArchive?"Create archive":s.modal==Modal::ExtractArchive?"Extract archive":s.modal==Modal::ConvertImage?"Convert image":s.modal==Modal::Command?"Run command here":s.modal==Modal::Help?"Keyboard shortcuts":s.modal==Modal::About?"About raymothfm":s.modal==Modal::ThemePicker?"Theme picker":s.modal==Modal::PasteOverwrite?"Confirm overwrite":s.modal==Modal::ConfirmPermanentDelete?"Permanent delete":s.modal==Modal::Checksum?"Checksum":"raymothfm";
    DrawText(title.c_str(),(int)box.x+20,(int)box.y+18, uiFont(20), t.text);



    if (s.modal==Modal::Checksum) {
        DrawText(s.checksumName.c_str(), (int)box.x+20, (int)box.y+68, uiFont(14), t.text);
        DrawText(s.checksumPath.c_str(), (int)box.x+20, (int)box.y+95, uiFont(11), t.muted);
        DrawText("Digest:", (int)box.x+20, (int)box.y+145, uiFont(13), t.muted);
        Rectangle hashR{box.x+20, box.y+172, box.width-40, 42};
        DrawRectangleRec(hashR, t.panel); DrawRectangleLinesEx(hashR, 1, t.line);
        const bool good = s.checksumValue.rfind("ERROR:",0) != 0 && s.checksumValue != "Calculating...";
        DrawText(s.checksumValue.c_str(), (int)hashR.x+10, (int)hashR.y+12, uiFont(14), good ? t.text : t.muted);
        DrawText("Click the digest to copy it to the clipboard", (int)box.x+20, (int)box.y+235, uiFont(12), t.muted);
        Rectangle closeR{box.x+440,box.y+344,90,32}; DrawRectangleRec(closeR,t.panel); DrawRectangleLinesEx(closeR, 1, t.line); DrawText("Close",(int)closeR.x+24,(int)closeR.y+9, uiFont(13), t.text);
        return;
    }

    if (s.modal==Modal::ConfirmPermanentDelete) {
        const size_t count = s.pendingPermanentDelete.size();
        DrawText("This action cannot be undone.", (int)box.x+20, (int)box.y+70, uiFont(15), t.text);
        DrawText((std::to_string(count) + (count == 1 ? " item will be permanently deleted." : " items will be permanently deleted.")).c_str(),
                 (int)box.x+20, (int)box.y+100, uiFont(13), t.muted);
        if (!s.pendingPermanentDelete.empty()) {
            std::string label = s.pendingPermanentDelete.front().filename().string();
            if (count > 1) label += "  (and " + std::to_string(count-1) + " more)";
            DrawText(label.c_str(), (int)box.x+20, (int)box.y+130, uiFont(12), t.muted);
        }
        Rectangle apply{box.x+350,box.y+180,110,34}, cancel{box.x+470,box.y+180,70,34};
        DrawRectangleRec(apply, t.accent); DrawText("Delete", (int)apply.x+29, (int)apply.y+9, uiFont(13), t.bg);
        DrawRectangleLinesEx(cancel, 1, t.line); DrawText("Cancel", (int)cancel.x+12, (int)cancel.y+9, uiFont(12), t.text);
        DrawText("Enter = delete permanently    Esc = cancel", (int)box.x+20, (int)box.y+240, uiFont(12), t.muted);
        return;
    }

    if (s.modal==Modal::PasteOverwrite) {
        DrawText("A file or directory with this name already exists.", (int)box.x+20,(int)box.y+68, uiFont(14), t.text);
        DrawText(s.pendingPasteDestination.filename().string().c_str(), (int)box.x+20,(int)box.y+98, uiFont(13), t.muted);
        DrawText("Replace the existing item?", (int)box.x+20,(int)box.y+126, uiFont(13), t.text);
        Rectangle apply{box.x+360,box.y+180,100,32}, cancel{box.x+470,box.y+180,70,32};
        DrawRectangleRec(apply,t.accent); DrawText("Overwrite",(int)apply.x+10,(int)apply.y+8, uiFont(12), t.bg);
        DrawRectangleLinesEx(cancel, 1, t.line); DrawText("Cancel",(int)cancel.x+12,(int)cancel.y+8, uiFont(12), t.text);
        return;
    }

    if (s.modal==Modal::ThemePicker) {
        DrawText("Theme / Appearance",(int)box.x+20,(int)box.y+50, uiFont(15), t.accent);
        auto field=[&](Rectangle rr,const char* label,TextField tf,const std::string& value){
            DrawText(label,(int)rr.x,(int)rr.y-16,uiFont(10),t.muted);
            DrawRectangleRec(rr,t.bg); DrawRectangleLinesEx(rr,1,s.focusedField==tf?t.accent:t.line);
            if(s.focusedField==tf) drawInlineEditor(rr,value,s.editor,t,13); else {
                BeginScissorMode((int)rr.x+1,(int)rr.y+1,std::max(1,(int)rr.width-2),std::max(1,(int)rr.height-2));
                DrawText(value.c_str(),(int)rr.x+8,(int)rr.y+9,uiFont(13),t.text);
                EndScissorMode();
            }
        };
        field({box.x+20,box.y+92,150,34},"Theme number (0-10)",TextField::ThemeNumber,s.themeEdit);
        field({box.x+20,box.y+152,270,34},"EDITOR",TextField::EditorConfig,s.editorEdit);
        field({box.x+20,box.y+212,270,34},"TERM",TextField::TermConfig,s.termEdit);
        field({box.x+20,box.y+272,170,34},"Font scale (1.0-2.5)",TextField::FontScale,s.fontScaleEdit);
        field({box.x+200,box.y+272,170,34},"Accent #RRGGBB",TextField::AccentConfig,s.accentEdit);
        Rectangle hsvBtn{box.x+380,box.y+272,62,34}; DrawRectangleRec(hsvBtn,t.panel); DrawRectangleLinesEx(hsvBtn,1,t.accent); DrawText("HSV",(int)hsvBtn.x+17,(int)hsvBtn.y+9,uiFont(11),t.text);
        DrawText("Themes",(int)box.x+330,(int)box.y+76,uiFont(12),t.muted);
        int yy=(int)box.y+100; for(int i=0;i<(int)kThemes.size();++i){ Color c=(i==s.config.theme)?t.accent:t.text; DrawText((std::to_string(i)+"  "+kThemes[i].name).c_str(),(int)box.x+330,yy,uiFont(10),c); yy+=17; }
        Rectangle apply{box.x+390,box.y+360,85,32},cancel{box.x+485,box.y+360,65,32};
        DrawRectangleRec(apply,t.panel); DrawRectangleLinesEx(apply,1,t.accent); DrawText("Apply",(int)apply.x+20,(int)apply.y+9,uiFont(13),t.text);
        DrawRectangleRec(cancel,t.panel); DrawRectangleLinesEx(cancel,1,t.line); DrawText("Cancel",(int)cancel.x+10,(int)cancel.y+9,uiFont(13),t.text);
        DrawText("Enter = apply    Esc = cancel    HSV = random accent",(int)box.x+20,(int)box.y+360,uiFont(10),t.muted);
        return;
    }

    if (s.modal==Modal::Help) {
        DrawText("Navigation",(int)box.x+20,(int)box.y+58, uiFont(15), t.accent);
        const char* lines[] = {
            "/                 Focus path (keep text)", "Ctrl+/             Clear + focus path", "Ctrl+L             Focus/select path", "Tab                Complete path/command", "Enter              Open / activate", "Arrow keys         Navigate selection", "PageUp/PageDown    Page through items", "Backspace          Parent directory", "Alt+Left/Right     History", "Ctrl+T/W/Tab        New/close/switch tab",
            "Ctrl+1..9          Switch tab", "Ctrl+F             Focus search", "Ctrl+H             Show/hide hidden files", "Ctrl+A             Select all", "Ctrl+C/X/V         Copy/cut/paste", "Delete / Shift+Del Trash / permanent delete", "F2                 Rename", "F7 / Ctrl+Shift+N   New directory", "Ctrl+Alt+N          New file", "F6                 cat / text viewer", "F8                 Copy current directory path", "F9                 Open terminal here", "Ctrl+Home          Jump to Home", "A-Z                Focus search and start typing", "Ctrl+Shift+A        Compress", "X                  Extract selected archive", "Ctrl+Wheel          Change view zoom", "1..4               Details/List/Medium/Large", "F3                 Theme picker (0..10)", "`                  Run command here", "Tab                Path/command completion", "F1                 This help", "F4                About", "Config: show_thumbnails=1 + thumbnail_res=1..5 (32/64/128/256/512px); thumbnails use a visible-set-aware bounded cache (96 medium / 48 large).", "Ctrl+Q             Quit", "Esc                Close active window/menu"
        };
        int y=(int)box.y+86; for(const char* line:lines){ DrawText(line,(int)box.x+20,y, uiFont(12), t.text); y+=14; if(y>box.y+392) break; }
        DrawText("Details view always shows Name, Date modified, Type and Size.",(int)box.x+20,(int)box.y+402, uiFont(12), t.muted);
        return;
    }
    if (s.modal==Modal::About) {
        DrawText("raymothfm",(int)box.x+20,(int)box.y+72, uiFont(30), t.text);
        DrawText("A fast Linux file manager built with raylib.",(int)box.x+20,(int)box.y+112, uiFont(14), t.muted);
        DrawText("VFS / libarchive / libmagic / XDG integration / 7z / OpenSSL / libvips",(int)box.x+20,(int)box.y+138, uiFont(13), t.text);
        DrawText(RAYMOTHFM_VERSION,(int)box.x+20,(int)box.y+164, uiFont(13), t.text);
        DrawText("Made with <3 by @HalanoSiblee The Smart Moth",(int)box.x+20,(int)box.y+190, uiFont(13), t.text);
        DrawText("F1 = shortcuts    Esc = close",(int)box.x+20,(int)box.y+220, uiFont(13), t.muted);
        return;
    }

    if (s.modal==Modal::ImageView) {
        if(s.imageTexture.id){
            float scale=s.flags.imageFit?std::min((box.width-40)/s.imageW,(box.height-80)/s.imageH):s.imageZoom;
            scale=std::max(0.05f,scale); float dw=s.imageW*scale, dh=s.imageH*scale; Rectangle dst{box.x+(box.width-dw)/2+s.imagePan.x,box.y+60+(box.height-80-dh)/2+s.imagePan.y,dw,dh}; DrawTexturePro(s.imageTexture,{0,0,(float)s.imageTexture.width,(float)s.imageTexture.height},dst,{0,0}, uiFont(0), WHITE);
            char info[160]; std::snprintf(info,sizeof(info),"%dx%d  %.0f%%   +/- / wheel = zoom   drag = pan   F = fit   Esc = close",s.imageW,s.imageH,scale*100.0f); DrawText(info,(int)box.x+20,(int)(box.y+box.height-24), uiFont(12), t.muted);
        }
        return;
    }

    if (s.modal==Modal::ConvertImage) {
        DrawText("Input",(int)box.x+20,(int)box.y+52, uiFont(12), t.muted);
        BeginScissorMode((int)box.x+18,(int)box.y+62,(int)box.width-36,20);
        DrawText(s.convertInput.c_str(),(int)box.x+20,(int)box.y+71, uiFont(12), t.text);
        EndScissorMode();
        DrawText("Output",(int)box.x+20,(int)box.y+96, uiFont(12), t.muted);
        Rectangle out{box.x+20,box.y+106,box.width-40,38}; DrawRectangleRec(out,t.bg); DrawRectangleLinesEx(out,1,s.focusedField==TextField::ConvertOutput?t.accent:t.line);
        if(s.focusedField==TextField::ConvertOutput) {
            drawInlineEditor(out,s.convertOutput,s.editor,t,13);
        } else {
            BeginScissorMode((int)out.x+8,(int)out.y+3,(int)out.width-16,(int)out.height-6);
            DrawText(s.convertOutput.c_str(),(int)out.x+8,(int)out.y+11, uiFont(13), t.text);
            EndScissorMode();
        }
        const char* labels[]={"Format","Quality","Effort","Compression","Bit depth","Lossless","Strip metadata"};
        for(int i=0;i<7;++i){
            int col=i%2,row=i/2; float bx=box.x+20+col*285, by=box.y+160+row*42;
            bool enabled=true;
            if(i==1) enabled=convertQualityEnabled(s);
            else if(i==2) enabled=convertEffortEnabled(s);
            else if(i==3) enabled=convertCompressionEnabled(s);
            else if(i==4) enabled=convertBitDepthEnabled(s);
            else if(i==5) enabled=convertLosslessEnabled(s);
            Color tc=enabled?t.text:t.muted;
            DrawText(labels[i],(int)bx,(int)by, uiFont(11), enabled?t.muted:t.line);
            Rectangle rr{bx,by+12,255,28}; DrawRectangleRec(rr,enabled?t.panel:Fade(t.panel,0.55f));
            DrawRectangleLinesEx(rr,1,s.modalField==i? (enabled?t.accent:t.line):t.line);
            DrawText(convertFieldValue(s,i),(int)rr.x+8,(int)rr.y+7, uiFont(13), tc);
        }
        Rectangle apply{box.x+340,box.y+360,110,34},cancel{box.x+460,box.y+360,90,34}; DrawRectangleRec(apply,t.accent); DrawText("Convert",(int)apply.x+27,(int)apply.y+10, uiFont(13), t.bg); DrawRectangleRec(cancel,t.panel); DrawRectangleLinesEx(cancel, 1, t.line); DrawText("Cancel",(int)cancel.x+22,(int)cancel.y+10, uiFont(13), t.text);
        const int errorY = !s.modalError.empty() ? (int)box.y+327 : -1;
        if(errorY >= 0) {
            BeginScissorMode((int)box.x+18,errorY,(int)box.width-36,16);
            DrawText(s.modalError.c_str(),(int)box.x+20,errorY, uiFont(10), Color{255,100,100,255});
            EndScissorMode();
        }
        const int helpY = !s.modalError.empty() ? (int)box.y+344 : (int)box.y+330;
        DrawText("Click parameter: cycle    LMB/RMB = next/previous    Enter = convert    Esc = cancel",(int)box.x+20,helpY, uiFont(11), t.muted);
        return;
    }

    if (s.modal==Modal::Command) {
        DrawText("Current directory",(int)box.x+20,(int)box.y+55, uiFont(12), t.muted);
        DrawText(s.path.c_str(),(int)box.x+20,(int)box.y+74, uiFont(12), t.text);
        Rectangle field{box.x+20,box.y+100,box.width-40,38};
        DrawRectangleRec(field,t.bg); DrawRectangleLinesEx(field, 1, t.accent);
        const std::string& v=s.commandEdit;
        DrawText(v.c_str(),(int)field.x+8,(int)field.y+10, uiFont(14), t.text);
        if(s.editor.hasSelection()){ const int a=MeasureText(v.substr(0,s.editor.lo()).c_str(),uiFont(14)), b=MeasureText(v.substr(0,s.editor.hi()).c_str(),uiFont(14)); DrawRectangle((int)field.x+8+a,(int)field.y+8,b-a,17,Fade(t.accent,0.35f)); }
        else { const int tw=MeasureText(v.substr(0,s.editor.cursor).c_str(),uiFont(14)); DrawLine((int)field.x+8+tw,(int)field.y+7,(int)field.x+8+tw,(int)field.y+30,t.accent); }
        DrawText("Enter = run    Esc = cancel    Ctrl+A/C/X/V = edit",(int)box.x+20,(int)box.y+160, uiFont(12), t.muted);
        return;
    }

    if (s.modal==Modal::Rename) {
        DrawText("New name",(int)box.x+20,(int)box.y+70, uiFont(13), t.muted);
        Rectangle field{box.x+20,box.y+95,box.width-240,38}; DrawRectangleRec(field,t.bg); DrawRectangleLinesEx(field, 1, t.accent);
        const std::string& v=s.renameEdit; DrawText(v.c_str(),(int)field.x+8,(int)field.y+10, uiFont(14), t.text);
        if (s.focusedField==TextField::Rename) {
            const int tw=MeasureText(v.substr(0,s.editor.cursor).c_str(),uiFont(14)); DrawLine((int)field.x+8+tw,(int)field.y+7,(int)field.x+8+tw,(int)field.y+30,t.accent);
        }
        if (s.editor.hasSelection()) { const int a=MeasureText(v.substr(0,s.editor.lo()).c_str(),uiFont(14)), b=MeasureText(v.substr(0,s.editor.hi()).c_str(),uiFont(14)); DrawRectangle((int)field.x+8+a,(int)field.y+8,b-a,17,Fade(t.accent,0.35f)); DrawText(v.substr(s.editor.lo(),s.editor.hi()-s.editor.lo()).c_str(),(int)field.x+8+a,(int)field.y+10, uiFont(14), t.text); }
        DrawText("Enter = rename    Esc = cancel",(int)box.x+20,(int)box.y+155, uiFont(13), t.muted);
        DrawRectangleRec({box.x+390,box.y+105,85,32},t.panel); DrawRectangleLinesEx({box.x+390,box.y+105,85,32}, 1, t.line); DrawText("Rename",(int)box.x+408,(int)box.y+114, uiFont(13), t.text);
        DrawRectangleRec({box.x+485,box.y+105,65,32},t.panel); DrawRectangleLinesEx({box.x+485,box.y+105,65,32}, 1, t.line); DrawText("Cancel",(int)box.x+496,(int)box.y+114, uiFont(13), t.text);
        if (!s.modalError.empty()) DrawText(s.modalError.c_str(),(int)box.x+20,(int)box.y+190, uiFont(13), Color{255,100,100,255});
        return;
    }

    if (s.modal==Modal::NewItem) {
        DrawText(s.flags.newItemDirectory ? "Directory name" : "File name", (int)box.x+20, (int)box.y+70, uiFont(13), t.muted);
        Rectangle field{box.x+20,box.y+95,box.width-240,38};
        DrawRectangleRec(field,t.bg); DrawRectangleLinesEx(field, 1, t.accent);
        const std::string& v=s.newItemEdit;
        DrawText(v.c_str(),(int)field.x+8,(int)field.y+10, uiFont(14), t.text);
        if (s.editor.hasSelection()) {
            const int a=MeasureText(v.substr(0,s.editor.lo()).c_str(),uiFont(14));
            const int b=MeasureText(v.substr(0,s.editor.hi()).c_str(),uiFont(14));
            DrawRectangle((int)field.x+8+a,(int)field.y+8,b-a,17,Fade(t.accent,0.35f));
            DrawText(v.substr(s.editor.lo(),s.editor.hi()-s.editor.lo()).c_str(),(int)field.x+8+a,(int)field.y+10, uiFont(14), t.text);
        } else if (s.focusedField==TextField::NewItem) {
            const int tw=MeasureText(v.substr(0,s.editor.cursor).c_str(),uiFont(14));
            DrawLine((int)field.x+8+tw,(int)field.y+7,(int)field.x+8+tw,(int)field.y+30,t.accent);
        }
        DrawText("Enter = create    Esc = cancel",(int)box.x+20,(int)box.y+155, uiFont(13), t.muted);
        DrawRectangleRec({box.x+390,box.y+105,85,32},t.panel); DrawRectangleLinesEx({box.x+390,box.y+105,85,32}, 1, t.accent); DrawText("Create",(int)box.x+407,(int)box.y+114, uiFont(13), t.text);
        DrawRectangleRec({box.x+485,box.y+105,65,32},t.panel); DrawRectangleLinesEx({box.x+485,box.y+105,65,32}, 1, t.line); DrawText("Cancel",(int)box.x+496,(int)box.y+114, uiFont(13), t.text);
        if (!s.modalError.empty()) DrawText(s.modalError.c_str(),(int)box.x+20,(int)box.y+190, uiFont(13), Color{255,100,100,255});
        return;
    }


    if (s.modal==Modal::CreateArchive || s.modal==Modal::ExtractArchive) {
        if (s.modal==Modal::ExtractArchive) {
            DrawText("Destination",(int)box.x+20,(int)box.y+54, uiFont(12), t.muted);
            Rectangle dest{box.x+20,box.y+74,box.width-40,34}; DrawRectangleRec(dest,t.bg); DrawRectangleLinesEx(dest,1,s.focusedField==TextField::ExtractDestination?t.accent:t.line);
            if(s.focusedField==TextField::ExtractDestination) drawInlineEditor(dest,s.archive.destination,s.editor,t,13); else DrawText(s.archive.destination.c_str(),(int)dest.x+8,(int)dest.y+9, uiFont(13), t.text);
            Rectangle pw{box.x+20,box.y+120,box.width-40,34}; DrawRectangleRec(pw,t.bg); DrawRectangleLinesEx(pw,1,s.focusedField==TextField::ArchivePassword?t.accent:t.line);
            std::string mask(s.archive.password.size(), '*'); if(s.focusedField==TextField::ArchivePassword) drawInlineEditor(pw,mask.empty()?std::string{}:mask,s.editor,t,13); else DrawText(mask.empty()?"Password (optional)":mask.c_str(),(int)pw.x+8,(int)pw.y+9,uiFont(13),mask.empty()?t.muted:t.text);
            Rectangle ow{box.x+20,box.y+166,210,28};
            DrawRectangleLinesEx(ow, 1, t.line);
            DrawText((std::string("Overwrite: ")+(s.archive.overwrite?"YES":"NO")).c_str(),(int)ow.x+8,(int)ow.y+7, uiFont(13), t.text);
            Rectangle apply{box.x+340,box.y+360,110,34},cancel{box.x+460,box.y+360,90,34};
            DrawRectangleRec(apply,t.panel); DrawRectangleLinesEx(apply, 1, t.accent); DrawText("Extract",(int)apply.x+29,(int)apply.y+10, uiFont(13), t.text);
            DrawRectangleRec(cancel,t.panel); DrawRectangleLinesEx(cancel, 1, t.line); DrawText("Cancel",(int)cancel.x+22,(int)cancel.y+10, uiFont(13), t.text);
            DrawText("Enter = extract    Tab = next field    Esc = cancel",(int)box.x+20,(int)box.y+210, uiFont(13), t.muted);
            if(!s.modalError.empty()) DrawText(s.modalError.c_str(),(int)box.x+20,(int)box.y+246, uiFont(12), Color{255,100,100,255});
            return;
        }
        DrawText("Output",(int)box.x+20,(int)box.y+48, uiFont(12), t.muted);
        Rectangle out{box.x+20,box.y+62,box.width-40,34}; DrawRectangleRec(out,t.bg); DrawRectangleLinesEx(out,1,s.focusedField==TextField::ArchiveOutput?t.accent:t.line); if(s.focusedField==TextField::ArchiveOutput) drawInlineEditor(out,s.archive.output,s.editor,t,13); else DrawText(s.archive.output.c_str(),(int)out.x+8,(int)out.y+9, uiFont(13), t.text);
        const int x1=(int)box.x+20, x2=(int)box.x+305, yy=(int)box.y+114;
        DrawText((std::string("Format: ") + archiveFormatName(s.archive.format)).c_str(),x1,yy, uiFont(13), t.text);
        DrawText((std::string("Compression: ") + compressionName(s.archive.compression)).c_str(),x2,yy, uiFont(13), t.text);
        DrawText(("Level: "+std::to_string(s.archive.level)).c_str(),x1,yy+30, uiFont(13), t.text);
        DrawText(("Threads: "+std::to_string(s.archive.threads)).c_str(),x2,yy+30, uiFont(13), t.text);
        DrawText(("Method: "+s.archive.sevenZipMethod).c_str(),x1,yy+60, uiFont(13), t.text);
        DrawText(("Encryption: "+std::string(archiveEncryptionLabel(s.archive))).c_str(),x2,yy+60,uiFont(13),archivePasswordUIEnabled(s.archive)?t.text:t.muted);
        const bool pwEnabled=archivePasswordUIEnabled(s.archive);
        Rectangle pw{x1*1.0f,box.y+236,box.width-40,34}; DrawRectangleRec(pw,pwEnabled?t.bg:t.panel); DrawRectangleLinesEx(pw,1,(pwEnabled&&s.focusedField==TextField::ArchivePassword)?t.accent:t.line);
        std::string mask(s.archive.password.size(), '*');
        if(pwEnabled && s.focusedField==TextField::ArchivePassword) {
            // Render the actual editable password buffer without exposing it.
            std::string stars(s.archive.password.size(),'*'); drawInlineEditor(pw,stars,s.editor,t,13);
        } else DrawText((pwEnabled?(mask.empty()?"Password (optional)":mask):"Password unsupported by this format").c_str(),(int)pw.x+8,(int)pw.y+9,uiFont(13),pwEnabled?(mask.empty()?t.muted:t.text):t.muted);
        const char* archiveHelp = (s.archive.format==ArchiveFormat::SevenZip) ? "7z password uses the system 7z/7zz tool; libarchive handles the archive data." : "Click a parameter or use LMB/RMB to cycle it. Format selects a template; every parameter remains editable.";
        DrawText(archiveHelp,(int)box.x+20,(int)box.y+286, uiFont(11), t.muted);
        Rectangle apply{box.x+340,box.y+360,110,34},cancel{box.x+460,box.y+360,90,34};
        DrawRectangleRec(apply,t.panel); DrawRectangleLinesEx(apply, 1, t.accent); DrawText("Compress",(int)apply.x+21,(int)apply.y+10, uiFont(13), t.text);
        DrawRectangleRec(cancel,t.panel); DrawRectangleLinesEx(cancel, 1, t.line); DrawText("Cancel",(int)cancel.x+22,(int)cancel.y+10, uiFont(13), t.text);
        DrawText("Tab = next field   Left/Right = cycle   Enter = compress   Esc = cancel",(int)box.x+20,(int)box.y+306, uiFont(12), t.muted);
        if(!s.modalError.empty()) DrawText(s.modalError.c_str(),(int)box.x+20,(int)box.y+334, uiFont(12), Color{255,100,100,255});
        return;
    }

    if (s.modal==Modal::Cat) {
        DrawText("cat text view mode",(int)box.x+20,(int)box.y+54, uiFont(17), t.text);
        Rectangle view{box.x+18,box.y+72,box.width-36,box.height-110};
        DrawRectangleRec(view,colorHex(0x020202)); DrawRectangleLinesEx(view, 1, t.line);
        BeginScissorMode((int)view.x+1,(int)view.y+1,(int)view.width-2,(int)view.height-2);
        int y=(int)view.y+8;
        std::istringstream in(s.catText); std::string line; int n=0; const int lineH=16;
        while(std::getline(in,line)){ if(n++<s.catScroll) continue; DrawText(line.c_str(),(int)view.x+8,y, uiFont(12), t.text); y+=lineH; if(y>view.y+view.height-18) break; }
        EndScissorMode();
        DrawText("Up/Down/PageUp/PageDown or wheel = scroll    Esc = close",(int)box.x+20,(int)box.y+410, uiFont(11), t.muted);
        return;
    }


    if (s.modal==Modal::DiskInfo) {
        if(s.diskInfo.mounts.empty()){DrawText("No mounted filesystems",(int)box.x+20,(int)box.y+60, uiFont(14), t.text);return;}
        const int n=(int)s.diskInfo.mounts.size();
        const int tabLeft=(int)box.x+52, tabRight=(int)box.x+(int)box.width-52;
        const int tabAreaW=tabRight-tabLeft;
        const int tabW=132;
        const int visibleTabs=std::max(1,tabAreaW/tabW);
        s.diskInfo.firstTab=std::clamp(s.diskInfo.firstTab,0,std::max(0,n-visibleTabs));
        if(s.diskInfo.tab<s.diskInfo.firstTab) s.diskInfo.firstTab=s.diskInfo.tab;
        if(s.diskInfo.tab>=s.diskInfo.firstTab+visibleTabs) s.diskInfo.firstTab=s.diskInfo.tab-visibleTabs+1;
        Rectangle leftTab{box.x+20,box.y+18,26,28}, rightTab{box.x+box.width-46,box.y+18,26,28};
        DrawRectangleRec(leftTab,t.panel); DrawRectangleLinesEx(leftTab, 1, t.line); DrawText("<",(int)leftTab.x+8,(int)leftTab.y+6, uiFont(14), t.text);
        DrawRectangleRec(rightTab,t.panel); DrawRectangleLinesEx(rightTab, 1, t.line); DrawText(">",(int)rightTab.x+8,(int)rightTab.y+6, uiFont(14), t.text);
        BeginScissorMode(tabLeft,(int)box.y+16,tabAreaW,32);
        for(int slot=0;slot<visibleTabs && s.diskInfo.firstTab+slot<n;++slot){
            const int i=s.diskInfo.firstTab+slot;
            Rectangle tr{(float)(tabLeft+slot*tabW),(float)box.y+18,(float)tabW-4,28};
            DrawRectangleRec(tr,i==s.diskInfo.tab?t.panel:t.bg); DrawRectangleLinesEx(tr,1,i==s.diskInfo.tab?t.accent:t.line);
            std::string label=mountLabel(s.diskInfo.mounts[i]);
            if(label.size()>19) label=label.substr(0,16)+"...";
            DrawText(label.c_str(),(int)tr.x+7,(int)tr.y+7, uiFont(11), t.text);
        }
        EndScissorMode();
        const auto& d=s.diskInfo.mounts[std::clamp(s.diskInfo.tab,0,n-1)]; const int x=(int)box.x+20,y=(int)box.y+62;
        DrawText(d.source.c_str(),x,y, uiFont(14), t.text); DrawText(("Filesystem: "+d.fstype).c_str(),x,y+24, uiFont(12), t.muted); DrawText(("Mount point: "+d.mountpoint.string()).c_str(),x,y+44, uiFont(12), t.muted);
        const float ratio=d.total?std::clamp((float)((double)d.used/(double)d.total),0.0f,1.0f):0.0f; const Vector2 center{box.x+175,box.y+225};
        DrawCircle(center.x,center.y,104,t.panel); if(ratio>0)DrawCircleSector(center,104,-90,-90+ratio*360.0f, uiFont(96), t.accent); DrawCircle(center.x,center.y, uiFont(65), t.bg);
        DrawText(diskPercent(d).c_str(),(int)center.x-27,(int)center.y-10, uiFont(18), t.text); DrawText("used",(int)center.x-16,(int)center.y+13, uiFont(12), t.muted);
        const int sx=(int)box.x+325,sy=(int)box.y+150; DrawText(("Total: "+formatBytes(d.total)).c_str(),sx,sy, uiFont(14), t.text); DrawText(("Used:  "+formatBytes(d.used)).c_str(),sx,sy+32, uiFont(14), t.text); DrawText(("Free:  "+formatBytes(d.free)).c_str(),sx,sy+64, uiFont(14), t.text); DrawText(("Usage: "+diskPercent(d)).c_str(),sx,sy+96, uiFont(14), t.text);
        DrawText("Click a tab to inspect another mounted filesystem.",(int)box.x+20,(int)box.y+340, uiFont(12), t.muted); Rectangle closeR{box.x+440,box.y+344,70,32}; DrawRectangleRec(closeR,t.panel); DrawRectangleLinesEx(closeR, 1, t.line); DrawText("Close",(int)closeR.x+17,(int)closeR.y+9, uiFont(13), t.text); return;
    }

    if (s.modal==Modal::Properties) {
        if (s.props.multi) {
            DrawText("Multiple items", (int)box.x+20, (int)box.y+54, uiFont(18), t.text);
            DrawText((std::to_string(s.props.selectedCount)+" selected items").c_str(), (int)box.x+20, (int)box.y+84, uiFont(14), t.muted);
            DrawText(("Total size: "+formatBytes(s.props.totalSize)).c_str(), (int)box.x+20, (int)box.y+120, uiFont(15), t.text);
            DrawText(("Contained items: "+std::to_string(s.props.itemCount)).c_str(), (int)box.x+20, (int)box.y+148, uiFont(13), t.muted);
            DrawText("Selection summary; permissions are not editable for multiple items.", (int)box.x+20, (int)box.y+184, uiFont(12), t.muted);
            Rectangle closeR{box.x+440, box.y+210, 70, 32};
            DrawRectangleRec(closeR, t.panel); DrawRectangleLinesEx(closeR, 1, t.line);
            DrawText("Close", (int)closeR.x+16, (int)closeR.y+8, uiFont(13), t.text);
            return;
        }
        const fs::path p=s.props.path;
        DrawText(p.filename().string().c_str(),(int)box.x+20,(int)box.y+54, uiFont(18), t.text);
        DrawText(p.string().c_str(),(int)box.x+20,(int)box.y+77, uiFont(12), t.muted);
        DrawText(("Type: "+safeMagicOneLine(s.props.magic.description)).c_str(),(int)box.x+20,(int)box.y+108, uiFont(12), t.text);
        DrawText(("MIME: "+s.props.magic.mime).c_str(),(int)box.x+20,(int)box.y+129, uiFont(12), t.muted);
        DrawText(("Size: "+formatBytes(s.props.totalSize)).c_str(),(int)box.x+20,(int)box.y+151, uiFont(12), t.text);
        DrawText(("Items: "+std::to_string(s.props.itemCount)).c_str(),(int)box.x+250,(int)box.y+151, uiFont(12), t.muted);
        DrawText(("Modified: "+formatTime(s.props.mtime)).c_str(),(int)box.x+20,(int)box.y+173, uiFont(12), t.text);
        DrawText("Permissions",(int)box.x+20,(int)box.y+198, uiFont(15), t.text);
        DrawText("Octal",(int)box.x+20,(int)box.y+224, uiFont(12), t.muted);
        Rectangle modeR{box.x+68,box.y+217,90,31}; DrawRectangleRec(modeR,t.bg); DrawRectangleLinesEx(modeR,1,s.focusedField==TextField::Mode?t.accent:t.line); DrawText(s.props.modeEdit.c_str(),(int)modeR.x+8,(int)modeR.y+7, uiFont(14), t.text);
        DrawText("User",(int)box.x+35,(int)box.y+268, uiFont(12), t.muted); DrawText("Group",(int)box.x+205,(int)box.y+268, uiFont(12), t.muted); DrawText("Other",(int)box.x+375,(int)box.y+268, uiFont(12), t.muted);
        static const char* labels[3] = {"R","W","X"};
        static const mode_t bits[3][3]={{S_IRUSR,S_IWUSR,S_IXUSR},{S_IRGRP,S_IWGRP,S_IXGRP},{S_IROTH,S_IWOTH,S_IXOTH}};
        for(int c=0;c<3;++c) for(int r=0;r<3;++r) {
            Rectangle cb{box.x+25+c*170+r*44,box.y+290,16,16}; drawCheckbox(cb,(s.props.mode&bits[c][r])!=0,labels[r],t.text,t.accent);
        }
        DrawText("chmod is applied to the local filesystem item.",(int)box.x+20,(int)box.y+340, uiFont(12), t.muted);
        DrawText("Enter = edit octal field     Click Save to apply     Esc = cancel",(int)box.x+20,(int)box.y+370, uiFont(13), t.text);
        DrawRectangleRec({box.x+440,box.y+364,70,32},t.panel); DrawRectangleLinesEx({box.x+440,box.y+364,70,32}, 1, t.accent); DrawText("Save",(int)box.x+458,(int)box.y+373, uiFont(13), t.text);
        DrawRectangleRec({box.x+515,box.y+364,45,32},t.panel); DrawRectangleLinesEx({box.x+515,box.y+364,45,32}, 1, t.line); DrawText("Esc",(int)box.x+525,(int)box.y+373, uiFont(12), t.text);
        if (!s.modalError.empty()) DrawText(s.modalError.c_str(),(int)box.x+20,(int)box.y+402, uiFont(12), Color{255,100,100,255});
    }
}

static void drawContextMenu(const ExplorerState& s, const Theme& t, int W,int H) {
    if (!s.menu.open) return;
    const int itemH=30;
    std::string info;
    if (s.menu.row>=0 && s.menu.row<(int)s.rows.size() && s.vfs && s.vfs->isLocal()) info=safeMagicOneLine(s.magic.file(s.rows[s.menu.row].path).description);
    const int infoH=info.empty()?0:36;
    const int menuW=300, menuH=(int)s.menu.actions.size()*itemH+10+infoH;
    int x=(int)s.menu.pos.x, y=(int)s.menu.pos.y; if(x+menuW>W-4)x=W-menuW-4;if(y+menuH>H-4)y=H-menuH-4;
    Rectangle box{(float)x,(float)y,(float)menuW,(float)menuH}; DrawRectangleRec(box,t.panel2); DrawRectangleLinesEx(box, 1, t.line);
    const Vector2 mouse=GetMousePosition();
    for(int i=0;i<(int)s.menu.actions.size();++i){
        Rectangle r{(float)x+4,(float)y+5+i*itemH,(float)menuW-8,(float)itemH};
        if(pointIn(r,mouse)) DrawRectangleRounded(r,0.10f,4,Fade(t.accent,0.20f));
        DrawText(menuLabel(s.menu.actions[i]).c_str(),x+14,y+11+i*itemH, uiFont(13), t.text);
    }
    const int csi = checksumSubmenuIndex(s.menu);
    if (csi >= 0) {
        Rectangle parentR{(float)x+4,(float)y+5+csi*itemH,(float)menuW-8,(float)itemH};
        const float subW=240.0f, subH=5.0f*itemH+10.0f;
        const float subX=(x+menuW+subW<=W-4)?(float)(x+menuW):(float)(x-subW);
        const float subY=std::min(parentR.y, (float)H-subH-4.0f);
        Rectangle subR{subX, subY, subW, subH};
        if (pointIn(parentR, mouse) || pointIn(subR, mouse)) {
            DrawRectangleRec(subR, t.panel2); DrawRectangleLinesEx(subR, 1, t.line);
            for (int j=0;j<5;++j) {
                Rectangle sr{subR.x+4,subR.y+5+j*itemH,subR.width-8,(float)itemH};
                if(pointIn(sr,mouse)) DrawRectangleRounded(sr,0.10f,4,Fade(t.accent,0.20f));
                DrawText(checksumAlgorithmLabel(j), (int)sr.x+12, (int)sr.y+11, uiFont(13), t.text);
            }
        }
    }
    if(!info.empty()){ const int yy=y+10+(int)s.menu.actions.size()*itemH; DrawLine(x+8,yy,x+menuW-8,yy,t.line); DrawText("Type:",x+14,yy+8, uiFont(11), t.muted); DrawText(info.c_str(),x+48,yy+8, uiFont(11), t.text); }
}


static bool archivePasswordUIEnabled(const ArchiveOptions& o) {
    return o.format == ArchiveFormat::Zip || o.format == ArchiveFormat::SevenZip;
}

static const char* archiveEncryptionLabel(const ArchiveOptions& o) {
    if (o.format == ArchiveFormat::Zip) return o.encryption==0 ? "none" : o.encryption==1 ? "zipcrypt" : o.encryption==2 ? "aes128" : "aes256";
    if (o.format == ArchiveFormat::SevenZip) return o.encryption ? "AES-256 (7z)" : "none";
    return "unsupported";
}

static void applyArchiveTemplate(ExplorerState& s, bool updateOutput=true) {
    const std::string oldSuffix=s.archiveAutoSuffix;
    switch (s.archive.format) {
        case ArchiveFormat::Tar: s.archive.compression=ArchiveCompression::Gzip; s.archive.level=6; s.archive.threads=1; s.archive.sevenZipMethod="lzma2"; s.archive.encryption=0; break;
        case ArchiveFormat::Pax: s.archive.compression=ArchiveCompression::Gzip; s.archive.level=6; s.archive.threads=1; s.archive.sevenZipMethod="lzma2"; s.archive.encryption=0; break;
        case ArchiveFormat::Ustar: s.archive.compression=ArchiveCompression::Gzip; s.archive.level=6; s.archive.threads=1; s.archive.sevenZipMethod="lzma2"; s.archive.encryption=0; break;
        case ArchiveFormat::Zip: s.archive.compression=ArchiveCompression::None; s.archive.level=6; s.archive.threads=1; s.archive.sevenZipMethod="deflate"; s.archive.encryption=0; break;
        case ArchiveFormat::SevenZip: s.archive.compression=ArchiveCompression::None; s.archive.level=7; s.archive.threads=2; s.archive.sevenZipMethod="lzma2"; s.archive.encryption=0; break;
        case ArchiveFormat::Cpio: s.archive.compression=ArchiveCompression::None; s.archive.level=0; s.archive.threads=1; s.archive.sevenZipMethod="copy"; s.archive.encryption=0; break;
    }
    const std::string ns=defaultArchiveSuffix(s.archive);
    if (updateOutput && s.flags.archiveTemplateAuto && !s.archive.containerName.empty()) {
        fs::path out=s.archive.output;
        std::string fn=out.filename().string();
        if (!oldSuffix.empty() && fn==s.archive.containerName+oldSuffix) fn=s.archive.containerName+ns;
        else if (fn==s.archive.containerName || oldSuffix.empty()) fn=s.archive.containerName+ns;
        out=out.parent_path()/fn;
        s.archive.output=out.string();
    }
    s.archiveAutoSuffix=ns;
}

static void cycleArchiveField(ExplorerState& s,int delta){
    switch(s.modalField){
        case 1:{int x=(int)s.archive.format+delta;if(x<0)x=5;if(x>5)x=0;s.archive.format=(ArchiveFormat)x;applyArchiveTemplate(s,true);break;}
        case 2:{
            std::vector<ArchiveCompression> allowed;
            if(s.archive.format==ArchiveFormat::Zip || s.archive.format==ArchiveFormat::SevenZip || s.archive.format==ArchiveFormat::Cpio) allowed={ArchiveCompression::None};
            else allowed={ArchiveCompression::None,ArchiveCompression::Gzip,ArchiveCompression::Bzip2,ArchiveCompression::Xz,ArchiveCompression::Zstd,ArchiveCompression::Lz4,ArchiveCompression::Lzma,ArchiveCompression::Lzip};
            int cur=0; for(int i=0;i<(int)allowed.size();++i) if(allowed[i]==s.archive.compression){cur=i;break;}
            cur=(cur+delta+(int)allowed.size())%(int)allowed.size(); s.archive.compression=allowed[cur];
            const std::string old=s.archiveAutoSuffix; const std::string ns=defaultArchiveSuffix(s.archive); if(s.flags.archiveTemplateAuto&&!s.archive.containerName.empty()) {fs::path out=s.archive.output; if(out.filename().string()==s.archive.containerName+old || old.empty()) s.archive.output=(out.parent_path()/(s.archive.containerName+ns)).string();} s.archiveAutoSuffix=ns;
            break;
        }
        case 3:s.archive.level=std::clamp(s.archive.level+delta,0,9);break;
        case 4:s.archive.threads=std::clamp(s.archive.threads+delta,1,64);break;
        case 5:{static const char* m[] = {"copy","deflate","bzip2","lzma1","lzma2","zstd","ppmd"}; int idx=0;for(int i=0;i<7;++i)if(s.archive.sevenZipMethod==m[i])idx=i;idx=(idx+delta+7)%7;s.archive.sevenZipMethod=m[idx];break;}
        case 6: if(s.archive.format==ArchiveFormat::Zip) s.archive.encryption=(s.archive.encryption+delta+4)%4; else if(s.archive.format==ArchiveFormat::SevenZip) s.archive.encryption=(s.archive.encryption+delta+2)%2; else s.archive.encryption=0; break;
        default:break;
    }
}

static void openCreateArchive(ExplorerState& s) {
    const auto ids = selectedRows(s);
    if (ids.empty() || !s.vfs || !s.vfs->isLocal()) {
        s.status = "Select one or more local items to compress";
        return;
    }
    s.archive = ArchiveOptions{};
    s.archive.format = ArchiveFormat::Tar;
    s.archive.compression = ArchiveCompression::Gzip;
    s.archive.level = 6;
    s.archive.threads = 1;
    s.archive.sevenZipMethod = "lzma2";
    s.archive.password.clear(); s.archive.encryption=0;
    fs::path base = fs::path(s.path);
    std::string stem = "archive";
    if (ids.size() == 1 && ids.front() >= 0 && ids.front() < (int)s.rows.size()) {
        const fs::path selected = s.rows[ids.front()].path;
        stem = selected.filename().string();
        if (s.rows[ids.front()].kind == EntryKind::Archive) {
            stem = selected.stem().string();
        }
    }
    if (stem.empty()) stem = "archive";
    s.archive.containerName = stem;
    s.flags.archiveTemplateAuto = true;
    s.archiveAutoSuffix = defaultArchiveSuffix(s.archive);
    s.archive.output = (base / (stem + s.archiveAutoSuffix)).string();
    s.modalError.clear();
    s.modalField = 0;
    s.modal = Modal::CreateArchive;
    focus(s, TextField::ArchiveOutput, false);
    s.menu.open = false;
}

static bool createFromSelection(ExplorerState& s) {
    const auto ids = selectedRows(s);
    if (ids.empty() || !s.vfs || !s.vfs->isLocal()) {
        s.modalError = "Select at least one local item";
        return false;
    }
    fs::path output = fs::path(s.archive.output);
    if (output.empty()) {
        s.modalError = "Archive output path is empty";
        return false;
    }
    if (!output.is_absolute()) output = fs::path(s.path) / output;
    std::vector<fs::path> sources;
    sources.reserve(ids.size());
    for (int idx : ids) {
        if (idx >= 0 && idx < (int)s.rows.size()) sources.emplace_back(s.rows[idx].path);
    }
    if (sources.empty()) {
        s.modalError = "Nothing selected";
        return false;
    }
    std::error_code ec;
    if (fs::exists(output, ec)) {
        s.modalError = "Output already exists: " + output.string();
        return false;
    }
    std::string error;
    if (!createArchive(sources, fs::path(s.path), output, s.archive, error)) {
        s.modalError = error.empty() ? "Archive creation failed" : error;
        return false;
    }
    s.modal = Modal::None;
    unfocus(s);
    refresh(s);
    s.status = "Created archive: " + output.filename().string();
    return true;
}

static void openExtractArchive(ExplorerState& s) {
    const auto ids = selectedRows(s);
    if (ids.size() != 1 || !s.vfs || !s.vfs->isLocal()) {
        s.status = "Select one local archive to extract";
        return;
    }
    const VfsEntry& e = s.rows[ids.front()];
    if (e.kind != EntryKind::Archive) {
        s.status = "Selected item is not an archive";
        return;
    }
    // Standalone compression streams are not containers: extract them directly
    // with their system codec instead of ever routing them through libarchive.
    if (isSingleCompressionFile(e.path)) {
        extractSingleCompressionFile(s, fs::path(e.path), false);
        return;
    }
    s.archive = ArchiveOptions{};
    fs::path apath(e.path);
    std::string stem = apath.stem().string();
    if (stem.empty()) stem = "extracted";
    s.archive.destination = (fs::path(s.path) / stem).string();
    s.archive.overwrite = true;
    s.archive.password.clear();
    s.modalError.clear();
    s.modalField = 0;
    s.modal = Modal::ExtractArchive;
    focus(s, TextField::ExtractDestination, false);
    s.menu.open = false;
}

static bool extractCurrentArchive(ExplorerState& s) {
    const auto ids = selectedRows(s);
    if (ids.size() != 1 || !s.vfs || !s.vfs->isLocal()) {
        s.modalError = "Select one local archive";
        return false;
    }
    const VfsEntry& e = s.rows[ids.front()];
    if (e.kind != EntryKind::Archive) {
        s.modalError = "Selected item is not an archive";
        return false;
    }
    fs::path destination = s.archive.destination;
    if (destination.empty()) {
        s.modalError = "Extraction destination is empty";
        return false;
    }
    if (!destination.is_absolute()) destination = fs::path(s.path) / destination;
    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        s.modalError = "Cannot create destination: " + ec.message();
        return false;
    }
    std::string error;
    if (!extractArchive(fs::path(e.path), destination, s.archive.overwrite, s.archive.password, error)) {
        if (error.rfind("PASSWORD_REQUIRED:",0)==0) { s.archive.password.clear(); focus(s, TextField::ArchivePassword, false); s.modalError = "Password required. Enter it and press Enter."; return false; }
        s.modalError = error.empty() ? "Extraction failed" : error;
        return false;
    }
    s.modal = Modal::None;
    unfocus(s);
    refresh(s);
    s.status = "Extracted to: " + destination.filename().string();
    return true;
}

static void openNewItem(ExplorerState& s, bool directory) {
    if (!s.vfs || !s.vfs->isLocal() || !s.vfs->isDirectory(s.path)) {
        s.status = "New item requires a local directory";
        return;
    }
    s.flags.newItemDirectory = directory;
    s.newItemEdit.clear();
    s.modalError.clear();
    s.modal = Modal::NewItem;
    focus(s, TextField::NewItem, false);
    s.menu.open = false;
}

static bool createNewItem(ExplorerState& s) {
    if (!s.vfs || !s.vfs->isLocal() || !s.vfs->isDirectory(s.path)) {
        s.modalError = "Current location is not a local directory";
        return false;
    }
    std::string name = s.newItemEdit;
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) {
        s.modalError = "Invalid name";
        return false;
    }
    const fs::path target = fs::path(s.path) / name;
    std::error_code ec;
    if (s.flags.newItemDirectory) fs::create_directory(target, ec);
    else {
        std::ofstream out(target, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!out) ec = std::make_error_code(std::errc::io_error);
    }
    if (ec) {
        s.modalError = ec.message();
        return false;
    }
    s.modal = Modal::None;
    unfocus(s);
    refresh(s);
    s.status = std::string("Created ") + (s.flags.newItemDirectory ? "directory: " : "file: ") + name;
    return true;
}

static void processConvertModalMouse(ExplorerState& s, Vector2 mouse) {
    if(s.modal!=Modal::ConvertImage) return;
    const int W=GetScreenWidth(),H=GetScreenHeight(); Rectangle box{W*0.5f-290,H*0.5f-220,580,440}; if(!pointIn(box,mouse)) return;
    const bool l=IsMouseButtonPressed(MOUSE_BUTTON_LEFT), r=IsMouseButtonPressed(MOUSE_BUTTON_RIGHT); if(!(l||r)) return; const int d=r?1:-1;
    Rectangle out{box.x+20,box.y+106,box.width-40,38}; if(pointIn(out,mouse)){s.flags.convertOutputAuto=false; focusAt(s,TextField::ConvertOutput,mouse.x,out.x+8,13); return;}
    for(int i=0;i<7;++i){int col=i%2,row=i/2; Rectangle rr{box.x+20+col*285,box.y+172+row*42,255,28}; if(pointIn(rr,mouse)){s.modalField=i; if(i==0) { cycleConvertField(s,i,d); } else { cycleConvertField(s,i,d); } unfocus(s); return;}}
    Rectangle apply{box.x+340,box.y+360,110,34},cancel{box.x+460,box.y+360,90,34};
    if(l&&pointIn(apply,mouse)){ std::string err; if(convertImageFile(s,err)){s.modal=Modal::None;unfocus(s);refresh(s);s.status="Converted: "+fs::path(s.convertOutput).filename().string();} else s.modalError=err; }
    else if(l&&pointIn(cancel,mouse)){s.modal=Modal::None;unfocus(s);}
}

static void processArchiveModalMouse(ExplorerState& s, Vector2 mouse) {
    if (s.modal!=Modal::CreateArchive && s.modal!=Modal::ExtractArchive) return;
    const int W=GetScreenWidth(), H=GetScreenHeight();
    Rectangle box{W*0.5f-290,H*0.5f-220,580,440};
    if (!pointIn(box,mouse)) return;
    const bool clickL=IsMouseButtonPressed(MOUSE_BUTTON_LEFT), clickR=IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    const int delta=clickR?1:-1;
    if (!(clickL||clickR)) return;
    if (s.modal==Modal::ExtractArchive) {
        Rectangle dest{box.x+20,box.y+74,box.width-40,34};
        Rectangle pw{box.x+20,box.y+120,box.width-40,34};
        Rectangle ow{box.x+20,box.y+166,210,28};
        Rectangle apply{box.x+340,box.y+360,110,34}, cancel{box.x+460,box.y+360,90,34};
        if(pointIn(dest,mouse)){ focusAt(s,TextField::ExtractDestination,mouse.x,dest.x+8,13); return; }
        if(pointIn(pw,mouse)){ focusAt(s,TextField::ArchivePassword,mouse.x,pw.x+8,13); return; }
        if(pointIn(ow,mouse)){ s.archive.overwrite=!s.archive.overwrite; return; }
        if(clickL && pointIn(apply,mouse)){ extractCurrentArchive(s); return; }
        if(clickL && pointIn(cancel,mouse)){ s.modal=Modal::None; unfocus(s); return; }
        return;
    }
    Rectangle out{box.x+20,box.y+62,box.width-40,34};
    if(pointIn(out,mouse)){ s.flags.archiveTemplateAuto=false; focusAt(s,TextField::ArchiveOutput,mouse.x,out.x+8,13); return; }
    const int x1=(int)box.x+20, x2=(int)box.x+305, yy=(int)box.y+114;
    Rectangle rows[] = {
        {(float)x1,(float)yy-5,270,24}, {(float)x2,(float)yy-5,250,24},
        {(float)x1,(float)yy+25,270,24}, {(float)x2,(float)yy+25,250,24},
        {(float)x1,(float)yy+55,270,24}, {(float)x2,(float)yy+55,250,24}
    };
    for(int i=0;i<6;++i) if(pointIn(rows[i],mouse)){ s.modalField=i+1; cycleArchiveField(s,delta); return; }
    Rectangle pw{box.x+20,box.y+236,box.width-40,34};
    Rectangle apply{box.x+340,box.y+360,110,34}, cancel{box.x+460,box.y+360,90,34};
    if(pointIn(pw,mouse) && archivePasswordUIEnabled(s.archive)){ focusAt(s,TextField::ArchivePassword,mouse.x,pw.x+8,13); s.modalField=7; return; }
    if(clickL && pointIn(apply,mouse)){ createFromSelection(s); return; }
    if(clickL && pointIn(cancel,mouse)){ s.modal=Modal::None; unfocus(s); return; }
}

static void processConvertModalInput(ExplorerState& s){
    if(s.modal!=Modal::ConvertImage) return;
    if(s.focusedField==TextField::ConvertOutput) {
        if(IsKeyPressed(KEY_TAB)){ unfocus(s); return; }
        if(IsKeyPressed(KEY_ENTER)){ std::string err; if(convertImageFile(s,err)){s.modal=Modal::None;unfocus(s);refresh(s);s.status="Converted: "+fs::path(s.convertOutput).filename().string();} else s.modalError=err; }
        return;
    }
    if(IsKeyPressed(KEY_LEFT)) cycleConvertField(s,s.modalField,-1);
    if(IsKeyPressed(KEY_RIGHT)) cycleConvertField(s,s.modalField,1);
    if(IsKeyPressed(KEY_ENTER)){ std::string err; if(convertImageFile(s,err)){s.modal=Modal::None;unfocus(s);refresh(s);s.status="Converted: "+fs::path(s.convertOutput).filename().string();} else s.modalError=err; }
}

static void processArchiveModalInput(ExplorerState& s){
    if (s.modal == Modal::ExtractArchive) {
        if (IsKeyPressed(KEY_TAB)) {
            if (s.focusedField==TextField::ExtractDestination) focus(s,TextField::ArchivePassword,false);
            else focus(s,TextField::ExtractDestination,false);
        }
        if (IsKeyPressed(KEY_ENTER)) extractCurrentArchive(s);
        return;
    }
    if (s.focusedField==TextField::ArchiveOutput || s.focusedField==TextField::ArchivePassword) {
        if (IsKeyPressed(KEY_TAB)) {
            if (s.focusedField==TextField::ArchiveOutput && archivePasswordUIEnabled(s.archive)) focus(s,TextField::ArchivePassword,false);
            else unfocus(s);
            return;
        }
        if (IsKeyPressed(KEY_ENTER)) createFromSelection(s);
        return;
    }
    if (IsKeyPressed(KEY_TAB)) {
        s.modalField = (s.modalField + (isShiftDown() ? 5 : 1));
        if(s.modalField<1) s.modalField=6; if(s.modalField>6) s.modalField=1;
    }
    if (IsKeyPressed(KEY_LEFT)) cycleArchiveField(s, -1);
    if (IsKeyPressed(KEY_RIGHT)) cycleArchiveField(s, 1);
    if (IsKeyPressed(KEY_ENTER)) createFromSelection(s);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
#ifdef RAYMOTHFM_VERSION
            std::printf("raymothfm %s\n", RAYMOTHFM_VERSION);
#else
            std::printf("raymothfm unknown\n");
#endif
            return 0;
        }
        if (std::strcmp(argv[i], "--mkcfg") == 0) return writeTemplateConfig() ? 0 : 1;
        if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("raymothfm [--version] [--mkcfg]\n");
            return 0;
        }
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1360,780,"Win7RayExplorer / raymothfm");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    BeginDrawing();
    ClearBackground(BLACK);
    DrawText("Starting raymothfm...", 24, 24, 20, WHITE);
    DrawText("Initializing image/archive services...", 24, 54, 14, Color{170,170,170,255});
    EndDrawing();

    if (vips_init("raymothfm") != 0) return 1;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    // Thumbnail jobs run off the UI thread. Keep libvips worker width bounded to
    // avoid oversubscribing the machine when two decode jobs are active.
    vips_concurrency_set((int)std::clamp(hw >= 4 ? 2u : 1u, 1u, 2u));
    // We process many distinct images, so the libvips operation cache is not useful
    // for the proxy/thumbnail workload and can retain unnecessary state.
    vips_cache_set_max(0);
    ThumbnailWorker thumbnailWorker;
    thumbnailWorker.start(hw >= 4 ? 2 : 1);
    gThumbnailWorker = &thumbnailWorker;

    EnableEventWaiting();

    ExplorerState st;
    st.config=loadConfig();
    st.config.fontScale = std::clamp(st.config.fontScale, 1.0f, 2.5f);
    gUIFontScale = st.config.fontScale;
    st.config.accent = st.config.accent.a == 0 ? kThemes[st.config.theme].accent : st.config.accent;

    fs::path start=homeDir();
    openLocalPath(st,start);
    if (st.history.empty()) { st.history.push_back(start.string()); st.historyIndex=0; }
    st.tabs.resize(1);
    st.tabs[0] = saveTabSnapshot(st);
    restoreTabSnapshot(st,std::move(st.tabs[0]));

    for (;;) {
        pumpThumbnailResults(st);
        const bool navigationHeld =
            st.focusedField == TextField::None &&
            (IsKeyDown(KEY_UP) || IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_LEFT) ||
             IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_PAGE_UP) || IsKeyDown(KEY_PAGE_DOWN) ||
             IsKeyDown(KEY_HOME) || IsKeyDown(KEY_END));
        const bool thumbBusy = thumbnailWorker.hasPendingWork();
        if (thumbBusy || navigationHeld) {
            DisableEventWaiting();
            if (thumbBusy && !navigationHeld) WaitTime(0.004);
        } else EnableEventWaiting();
        if (WindowShouldClose()) break;
        if (isCtrlDown() && IsKeyPressed(KEY_Q)) break;
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (st.menu.open) st.menu.open=false;
            else if (st.modal!=Modal::None) { if (st.modal==Modal::ConfirmPermanentDelete) st.pendingPermanentDelete.clear(); st.modal=Modal::None; st.modalError.clear(); unfocus(st); }
            else if (st.focusedField!=TextField::None) unfocus(st);
        }
        const int W=GetScreenWidth(), H=GetScreenHeight();
        const Theme& baseTheme=kThemes[std::clamp(st.config.theme,0,(int)kThemes.size()-1)];
        Theme t=baseTheme; t.accent=st.config.accent;
        const Vector2 mouse=GetMousePosition();

        // Global hotkeys that should work outside text editors.
        if (st.modal==Modal::None) {
            if (isCtrlDown() && IsKeyPressed(KEY_SLASH)) { st.addressEdit.clear(); resetCompletion(st); focus(st,TextField::Address,false); }
            else if (!isCtrlDown() && !isAltDown() && IsKeyPressed(KEY_SLASH) && st.focusedField==TextField::None) { focus(st,TextField::Address,false); st.editor.cursor=st.addressEdit.size(); st.editor.anchor=st.editor.cursor; resetCompletion(st); }
            if (!isCtrlDown() && !isAltDown() && IsKeyPressed(KEY_GRAVE)) { st.commandEdit.clear(); st.modal=Modal::Command; focus(st,TextField::Command,false); st.menu.open=false; }
            if (IsKeyPressed(KEY_KB_MENU)) {
                if (st.menu.open) st.menu.open = false;
                else openContextMenu(st, st.selected, GetMousePosition());
            }
            if (IsKeyPressed(KEY_F1)) openHelp(st);
            if (IsKeyPressed(KEY_F4)) openAbout(st);
            if (IsKeyPressed(KEY_F3)) {
    st.themeEdit=std::to_string(st.config.theme); st.editorEdit=st.config.editor; st.termEdit=st.config.term; st.accentEdit=hexRGB(st.config.accent); st.fontScaleEdit=std::to_string(st.config.fontScale);
    st.modal=Modal::ThemePicker; focus(st,TextField::ThemeNumber,false); st.menu.open=false;
}
            if (isCtrlDown() && IsKeyPressed(KEY_F)) focus(st,TextField::Search,true);
            if (isCtrlDown() && IsKeyPressed(KEY_L)) focus(st,TextField::Address,true);
            if (isCtrlDown() && IsKeyPressed(KEY_T)) newTab(st,st.path.empty()?homeDir():fs::path(st.path));
            if (isCtrlDown() && IsKeyPressed(KEY_W)) closeActiveTab(st);
            if (isCtrlDown() && IsKeyPressed(KEY_TAB)) activateTab(st,isShiftDown()?st.activeTab-1:st.activeTab+1);
            if (isCtrlDown() && IsKeyPressed(KEY_H)) { st.config.showHidden=!st.config.showHidden; refresh(st); st.flags.configDirty=true; st.status=st.config.showHidden?"Hidden files shown":"Hidden files hidden"; }
            for (int k=0;k<9;++k) if (isCtrlDown() && IsKeyPressed(KEY_ONE+k) && k<(int)st.tabs.size()) activateTab(st,k);
            if (IsKeyPressed(KEY_F5) || (isCtrlDown() && IsKeyPressed(KEY_R))) refresh(st);
            if (IsKeyPressed(KEY_F8) && st.focusedField==TextField::None) {
                SetClipboardText(st.path.c_str());
                st.status = "Current directory path copied to clipboard";
            }
            if (IsKeyPressed(KEY_F9) && st.focusedField==TextField::None) {
                openCurrentDirectoryInTerminal(st);
            }
            if (isCtrlDown() && IsKeyPressed(KEY_HOME) && st.focusedField==TextField::None) {
                const fs::path hp = homeDir();
                std::error_code ec;
                if (st.vfs && st.vfs->isLocal() && fs::is_directory(hp, ec)) openLocalPath(st, hp);
                else if (st.vfs && st.vfs->isLocal()) st.status = "Home directory unavailable";
            }
            if (isAltDown() && IsKeyPressed(KEY_LEFT) && st.historyIndex>0) { --st.historyIndex; navigate(st,st.history[st.historyIndex],false); }
            if (isAltDown() && IsKeyPressed(KEY_RIGHT) && st.historyIndex+1<(int)st.history.size()) { ++st.historyIndex; navigate(st,st.history[st.historyIndex],false); }
            if (isAltDown() && IsKeyPressed(KEY_UP)) if (auto p=st.vfs->parent(st.path)) navigate(st,*p,true);
            if (st.focusedField==TextField::None && st.modal==Modal::None && !st.menu.open && !isCtrlDown() && !isAltDown()) {
                int cp = GetCharPressed();
                if (cp>0 && ((cp>='A' && cp<='Z') || (cp>='a' && cp<='z'))) {
                    focus(st,TextField::Search,false);
                    st.editor.replace(std::string(1,(char)cp));
                    updateFilter(st);
                }
            }
            if (st.focusedField==TextField::None && !isCtrlDown()) {
                if (IsKeyPressed(KEY_ONE)) st.view=ViewMode::Details;
                if (IsKeyPressed(KEY_TWO)) st.view=ViewMode::List;
                if (IsKeyPressed(KEY_THREE)) st.view=ViewMode::MediumIcons;
                if (IsKeyPressed(KEY_FOUR)) st.view=ViewMode::LargeIcons;

            }
            if (st.focusedField==TextField::None) {
                const int cols = gridColumns(st);
                if (keyRepeatPressed(KEY_UP)) moveSelection(st, (st.view==ViewMode::MediumIcons || st.view==ViewMode::LargeIcons) ? -cols : -1, false);
                if (keyRepeatPressed(KEY_DOWN)) moveSelection(st, (st.view==ViewMode::MediumIcons || st.view==ViewMode::LargeIcons) ? cols : 1, false);
                if (keyRepeatPressed(KEY_LEFT)) moveSelection(st,-1,false);
                if (keyRepeatPressed(KEY_RIGHT)) moveSelection(st,1,false);
                if (keyRepeatPressed(KEY_PAGE_UP)) moveSelection(st,-1,true);
                if (keyRepeatPressed(KEY_PAGE_DOWN)) moveSelection(st,1,true);
                if (keyRepeatPressed(KEY_HOME)) moveSelection(st,-100000,false);
                if (keyRepeatPressed(KEY_END)) moveSelection(st,100000,false);
                if (IsKeyPressed(KEY_ENTER)) openSelected(st);
            }
            if (isCtrlDown() && IsKeyPressed(KEY_A) && st.focusedField==TextField::None) selectAll(st);
            if (IsKeyPressed(KEY_F2) && st.focusedField==TextField::None) startRename(st);
            if (IsKeyPressed(KEY_F6) && st.focusedField==TextField::None) openCat(st);
            if (isCtrlDown() && isShiftDown() && IsKeyPressed(KEY_A) && st.focusedField==TextField::None) openCreateArchive(st);
            if (isCtrlDown() && isShiftDown() && IsKeyPressed(KEY_N) && st.focusedField==TextField::None) openNewItem(st, true);
            if (isCtrlDown() && isAltDown() && IsKeyPressed(KEY_N) && st.focusedField==TextField::None) openNewItem(st, false);
            if (IsKeyPressed(KEY_F7) && st.focusedField==TextField::None) openNewItem(st, true);
            if (IsKeyPressed(KEY_X) && st.focusedField==TextField::None && !isCtrlDown()) { if (selectedRows(st).size()==1 && st.rows[st.selected].kind==EntryKind::Archive) openExtractArchive(st); else if (!selectedRows(st).empty()) doCopyOrCut(st,true); }
            if (isCtrlDown() && IsKeyPressed(KEY_C) && st.focusedField==TextField::None) doCopyOrCut(st,false);
            if (isCtrlDown() && IsKeyPressed(KEY_V) && st.focusedField==TextField::None) pasteClipboard(st);
            if (isCtrlDown() && IsKeyPressed(KEY_X) && st.focusedField==TextField::None) doCopyOrCut(st,true);
            if (IsKeyPressed(KEY_DELETE) && st.focusedField==TextField::None) { if (isShiftDown()) openPermanentDeleteConfirm(st); else deleteSelection(st,false); }
            if (IsKeyPressed(KEY_BACKSPACE) && st.focusedField==TextField::None) if (auto p=st.vfs->parent(st.path)) navigate(st,*p,true);
        }

        if (st.modal==Modal::CreateArchive || st.modal==Modal::ExtractArchive) processArchiveModalInput(st);
        if (st.modal==Modal::PasteOverwrite && IsKeyPressed(KEY_ENTER)) {
            if (finishPasteOne(st, st.pendingPasteSource, st.pendingPasteDestination, st.flags.pendingPasteCut, true)) {
                ++st.pendingPasteIndex;
                st.modal = Modal::None;
                unfocus(st);
                continuePasteClipboard(st);
            }
        }
        if (st.modal==Modal::ConfirmPermanentDelete && IsKeyPressed(KEY_ENTER)) applyPermanentDelete(st);
        if (st.modal==Modal::NewItem && IsKeyPressed(KEY_ENTER)) { createNewItem(st); }
        if (st.modal==Modal::ImageView) {
            if(IsKeyPressed(KEY_F)) st.flags.imageFit=!st.flags.imageFit;
            if(IsKeyPressed(KEY_EQUAL)||IsKeyPressed(KEY_KP_ADD)) { st.flags.imageFit=false; st.imageZoom*=1.15f; }
            if(IsKeyPressed(KEY_MINUS)||IsKeyPressed(KEY_KP_SUBTRACT)) { st.flags.imageFit=false; st.imageZoom=std::max(0.05f,st.imageZoom/1.15f); }
            if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){ st.flags.imageDragging=true; st.imageDragStart=GetMousePosition(); st.imagePanStart=st.imagePan; }
            if(IsMouseButtonDown(MOUSE_BUTTON_LEFT) && st.flags.imageDragging){ Vector2 m=GetMousePosition(); st.flags.imageFit=false; st.imagePan={st.imagePanStart.x+(m.x-st.imageDragStart.x),st.imagePanStart.y+(m.y-st.imageDragStart.y)}; }
            if(IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) st.flags.imageDragging=false;
        }
        if (st.modal==Modal::Cat) {
            if (IsKeyPressed(KEY_UP)) st.catScroll=std::max(0,st.catScroll-1);
            if (IsKeyPressed(KEY_DOWN)) st.catScroll++;
            if (IsKeyPressed(KEY_PAGE_UP)) st.catScroll=std::max(0,st.catScroll-20);
            if (IsKeyPressed(KEY_PAGE_DOWN)) st.catScroll+=20;
        }

        if(st.modal==Modal::ConvertImage) processConvertModalInput(st);

        // Text editing. We use raylib input primitives but maintain cursor/selection ourselves.
        if (st.modal==Modal::None || st.modal==Modal::Rename || st.modal==Modal::NewItem || st.modal==Modal::Properties || st.modal==Modal::CreateArchive || st.modal==Modal::ExtractArchive || st.modal==Modal::ConvertImage || st.modal==Modal::Command || st.modal==Modal::ThemePicker || st.modal==Modal::PasteOverwrite) {
            if (st.focusedField!=TextField::None && st.editor.text) {
                if (IsKeyPressed(KEY_TAB) && (st.focusedField==TextField::Address || st.focusedField==TextField::Command) && st.modal!=Modal::CreateArchive) completeFocusedText(st);
                if (isCtrlDown() && IsKeyPressed(KEY_A)) st.editor.selectAll();
                if (isCtrlDown() && IsKeyPressed(KEY_C) && st.editor.hasSelection()) SetClipboardText(st.editor.text->substr(st.editor.lo(),st.editor.hi()-st.editor.lo()).c_str());
                if (isCtrlDown() && IsKeyPressed(KEY_X) && st.editor.hasSelection()) { SetClipboardText(st.editor.text->substr(st.editor.lo(),st.editor.hi()-st.editor.lo()).c_str()); st.editor.replace(""); }
                if (isCtrlDown() && IsKeyPressed(KEY_V)) { const char* clip=GetClipboardText(); if (clip) st.editor.replace(clip); }
                // Native-feeling typematic cursor motion for every textbox.
                // First press is immediate; a held key repeats after a short delay.
                static double leftRepeatAt = 0.0;
                static double rightRepeatAt = 0.0;
                const double now = GetTime();
                const bool shift = isShiftDown();
                const bool leftPressed = IsKeyPressed(KEY_LEFT);
                const bool rightPressed = IsKeyPressed(KEY_RIGHT);
                if (leftPressed) {
                    st.editor.moveLeft(shift);
                    leftRepeatAt = now + 0.22;
                } else if (IsKeyDown(KEY_LEFT)) {
                    if (leftRepeatAt == 0.0) leftRepeatAt = now + 0.22;
                    if (now >= leftRepeatAt) {
                        // Fast typematic repeat while held.
                        st.editor.moveLeft(shift);
                        leftRepeatAt = now + 0.025;
                    }
                } else {
                    leftRepeatAt = 0.0;
                }
                if (rightPressed) {
                    st.editor.moveRight(shift);
                    rightRepeatAt = now + 0.22;
                } else if (IsKeyDown(KEY_RIGHT)) {
                    if (rightRepeatAt == 0.0) rightRepeatAt = now + 0.22;
                    if (now >= rightRepeatAt) {
                        st.editor.moveRight(shift);
                        rightRepeatAt = now + 0.025;
                    }
                } else {
                    rightRepeatAt = 0.0;
                }
                if (IsKeyPressed(KEY_HOME)) { st.editor.cursor=0; if(!shift) st.editor.clear(); }
                if (IsKeyPressed(KEY_END)) { st.editor.cursor=st.editor.text->size(); if(!shift) st.editor.clear(); }
                const bool backspaceRepeat = keyRepeatPressed(KEY_BACKSPACE, 0.22, 0.025);
                if (isCtrlDown() && backspaceRepeat && (st.focusedField==TextField::Address || st.focusedField==TextField::Command)) st.editor.backspaceToSlash();
                else if (backspaceRepeat) st.editor.backspace();
                if (IsKeyPressed(KEY_DELETE)) st.editor.del();
                int cp=GetCharPressed();
                bool changed=false;
                while(cp>0) { if(cp>=32 && cp!=127) { st.editor.replace(std::string(1,(char)cp)); changed=true; } cp=GetCharPressed(); }
                if (changed || backspaceRepeat || IsKeyPressed(KEY_DELETE) || (isCtrlDown() && (IsKeyPressed(KEY_V)||IsKeyPressed(KEY_X)))) resetCompletion(st);
                if (st.focusedField==TextField::ArchiveOutput && (changed || backspaceRepeat || IsKeyPressed(KEY_DELETE) || (isCtrlDown() && (IsKeyPressed(KEY_V)||IsKeyPressed(KEY_X)))) ) st.flags.archiveTemplateAuto=false;
                if (st.focusedField==TextField::Search) updateFilter(st);
                if (IsKeyPressed(KEY_ENTER) && st.modal==Modal::None && st.focusedField==TextField::Search) { unfocus(st); resetCompletion(st); }
                if (IsKeyPressed(KEY_ENTER) && st.modal==Modal::None && st.focusedField==TextField::Address) {
                    fs::path p=st.addressEdit; std::error_code ec;
                    if (fs::is_directory(p,ec)) {
                        openLocalPath(st,p);
                    } else if (fs::is_regular_file(p,ec) && isSingleCompressionFile(p)) {
                        extractSingleCompressionFile(st,p,true);
                    } else if (fs::is_regular_file(p,ec) && hasArchiveExt(p.filename().string())) {
                        openArchive(st,p);
                    } else {
                        st.status="Path not found: "+st.addressEdit;
                    }
                    unfocus(st);
                }
                if (IsKeyPressed(KEY_ENTER) && st.modal==Modal::ThemePicker) {
                    const int n=std::atoi(st.themeEdit.c_str());
                    if(n>=0 && n<(int)kThemes.size()) { st.config.theme=n; if(!st.config.accentCustom) st.config.accent=kThemes[n].accent; st.flags.configDirty=true; st.status="Theme "+std::to_string(n)+": "+kThemes[n].name; st.modal=Modal::None; unfocus(st); }
                    else st.modalError="Theme must be a number from 0 to "+std::to_string((int)kThemes.size()-1);
                }
                if (IsKeyPressed(KEY_ENTER) && st.modal==Modal::Command) { if(!st.commandEdit.empty()){ if(st.vfs && st.vfs->isLocal() && spawnShellInDir(st.path,st.commandEdit,true)) st.status="Command started"; else st.status="Command requires a local filesystem directory"; st.modal=Modal::None; unfocus(st); } }
                if (IsKeyPressed(KEY_ENTER) && st.modal==Modal::Rename) applyRename(st);
                if (st.modal==Modal::Properties && st.focusedField==TextField::Mode && IsKeyPressed(KEY_ENTER)) { modeToBits(st.props); unfocus(st); }
            }
        }

        // Zoom: Ctrl + mouse wheel cycles view modes, wheel alone scrolls.
        const float wheel=GetMouseWheelMove();
        if (wheel!=0) {
            if (st.modal==Modal::Cat) st.catScroll=std::max(0,st.catScroll-(int)wheel*3);
            else if (st.modal==Modal::ImageView) { st.flags.imageFit=false; st.imageZoom=std::clamp(st.imageZoom*(wheel>0?1.12f:0.892857f),0.05f,20.0f); }
            else if (isCtrlDown()) {
                int v=(int)st.view; v += wheel>0 ? 1 : -1; v=std::clamp(v,1,4); st.view=(ViewMode)v;
            } else if (st.modal==Modal::None && st.menu.open==false) {
                st.scroll=std::max(0,st.scroll-(int)wheel*3);
            }
        }

        // Geometry.
        const int top=78, statusH=30, headerH=34;
        const int left=st.flags.sidebar ? (int)st.sidebarW : 0;
        Rectangle backR{8,9,34,31}, fwdR{46,9,34,31}, upR{84,9,34,31};
        Rectangle addrR{132,8,(float)(W-360),32};
        Rectangle searchR{W-220.0f,8,210,32};
        const int contentH=H-top-statusH;

        BeginDrawing();
        ClearBackground(t.bg);
        DrawRectangle(0,0,W,top,t.panel2); DrawLine(0,top,W,top,t.line);
        // Tab strip
        DrawRectangle(0,48,W, uiFont(30), t.panel); DrawLine(0,78,W, uiFont(78), t.line);
        const int tabW=170;
        for(int ti=0;ti<(int)st.tabs.size();++ti){
            Rectangle tr{(float)(8+ti*tabW),50,(float)tabW-4,26};
            const bool active=ti==st.activeTab;
            DrawRectangleRec(tr,active?t.panel2:t.panel);
            if(active) DrawRectangleLinesEx(tr, 1, t.accent); else DrawRectangleLinesEx(tr, 1, t.line);
            std::string title=tabTitle(st,ti); if(title.size()>20) title=title.substr(0,17)+"...";
            DrawText(title.c_str(),(int)tr.x+10,(int)tr.y+7, uiFont(12), t.text);
            if(ti>0 || st.tabs.size()>1) DrawText("x",(int)tr.x+tabW-24,(int)tr.y+6, uiFont(12), t.muted);
        }
        Rectangle plusR{(float)W-36.0f,51.0f,27.0f,24.0f}; DrawRectangleRec(plusR,t.panel); DrawRectangleLinesEx(plusR, 1, t.line); DrawText("+",W-29, 56, uiFont(16), t.text);
        DrawRectangleRec(backR,t.panel); DrawRectangleRec(fwdR,t.panel); DrawRectangleRec(upR,t.panel);
        DrawRectangleLinesEx(backR, 1, t.line); DrawRectangleLinesEx(fwdR, 1, t.line); DrawRectangleLinesEx(upR, 1, t.line);
        rayicons::Draw(rayicons::ArrowLeft,backR.x+9,backR.y+8, uiFont(1), t.muted); rayicons::Draw(rayicons::ArrowRight,fwdR.x+9,fwdR.y+8, uiFont(1), t.muted); rayicons::Draw(rayicons::ArrowUp,upR.x+9,upR.y+8, uiFont(1), t.muted);

        DrawRectangleRec(addrR,t.bg); DrawRectangleLinesEx(addrR,1,st.focusedField==TextField::Address?t.accent:t.line);
        if (st.focusedField==TextField::Address) drawInlineEditor(addrR,st.addressEdit,st.editor,t,14); else DrawText(st.addressEdit.c_str(),(int)addrR.x+10, 17, uiFont(14), t.text);
        DrawRectangleRec(searchR,t.bg); DrawRectangleLinesEx(searchR,1,st.focusedField==TextField::Search?t.accent:t.line);
        if(st.focusedField==TextField::Search) { if(st.searchEdit.empty()) DrawText("Search",(int)searchR.x+10, 17, uiFont(14), t.muted); drawInlineEditor(searchR,st.searchEdit,st.editor,t,14); }
        else DrawText(st.searchEdit.empty()?"Search":st.searchEdit.c_str(),(int)searchR.x+10,17,uiFont(14),st.searchEdit.empty()?t.muted:t.text);

        if (st.flags.sidebar) {
            DrawRectangle(0,top,left,contentH,t.panel); DrawLine(left,top,left,H-statusH,t.line);
            DrawText("Favorites", 15, top+17, uiFont(15), t.accent);
            int fy=top+45;
            for (size_t i=0;i<st.config.favorites.size();++i) {
                const fs::path fp=st.config.favorites[i]; const std::string label=fp.filename().empty()?fp.string():fp.filename().string();
                Rectangle fr{8.0f,(float)fy-5,left-16.0f,28};
                if (pointIn(fr,mouse)) DrawRectangleRec(fr,t.hover);
                rayicons::Draw(rayicons::FolderOpen,18,fy,1,(t.bg.r>240&&t.bg.g>240&&t.bg.b>240)?t.text:t.accent);
                std::string shown=label; if (shown.size()>25) shown=shown.substr(0,22)+"...";
                DrawText(shown.c_str(), 42, fy+3, uiFont(14), t.text); fy+=29;
            }
            DrawText("Computer", 15, fy+13, uiFont(15), t.accent); fy+=40;
            auto drawNavRow = [&](const char* label, const fs::path& target, rayicons::Icon icon)->Rectangle {
                Rectangle r{8.0f,(float)fy-5,left-16.0f,28};
                if(pointIn(r,mouse)) DrawRectangleRec(r,t.hover);
                rayicons::Draw(icon,18,fy,1,(t.bg.r>240&&t.bg.g>240&&t.bg.b>240)?t.text:t.accent);
                DrawText(label, 42, fy+3, uiFont(14), t.text);
                fy+=29;
                return r;
            };
            // Computer entries
            Rectangle homeR = drawNavRow("Home", homeDir(), rayicons::House);
            Rectangle rootR = drawNavRow("File System", fs::path("/"), rayicons::FolderOpen);
            Rectangle tmpR = drawNavRow("tmpfs (/tmp)", fs::path("/tmp"), rayicons::FolderOpen);
            (void)homeR; (void)rootR; (void)tmpR;
            DrawText("Disk", 15, fy+13, uiFont(15), t.accent); fy+=40; Rectangle diskR = drawNavRow("Disk usage", fs::path("/"), rayicons::FolderOpen); (void)diskR;
            DrawText("XDG", 15, fy+13, uiFont(15), t.accent); fy+=40;
            const fs::path dataHome=xdgDataHome();
            const fs::path configHome=configBase();
            const fs::path trashHome=xdgTrashHome();
            Rectangle trashR=drawNavRow("Trash", trashHome / "files", rayicons::FolderOpen);
            Rectangle dataR=drawNavRow("Data", dataHome, rayicons::FolderOpen);
            Rectangle cfgR=drawNavRow("Config", configHome, rayicons::FolderOpen);
            (void)trashR; (void)dataR; (void)cfgR;
        }

        Rectangle listR{(float)left,(float)top,(float)(W-left),(float)contentH}; DrawRectangleRec(listR,t.bg); DrawLine(left,top,left,H-statusH,t.line);
        DrawRectangle(left,top,W-left,headerH,t.panel); DrawLine(left,top+headerH,W,top+headerH,t.line);
        if (st.view==ViewMode::Details) {
            const char* arrows[4]={"", "", "", ""}; (void)arrows;
            DrawText((std::string("Name ")+(st.sortKey==SortKey::Name?(st.flags.sortAscending?"^":"v"):"" )).c_str(),left+42,top+11, uiFont(13), t.muted);
            DrawText((std::string("Date modified ")+(st.sortKey==SortKey::Date?(st.flags.sortAscending?"^":"v"):"" )).c_str(),left+500,top+11, uiFont(13), t.muted);
            DrawText((std::string("Type ")+(st.sortKey==SortKey::Type?(st.flags.sortAscending?"^":"v"):"" )).c_str(),left+700,top+11, uiFont(13), t.muted);
            DrawText((std::string("Size ")+(st.sortKey==SortKey::Size?(st.flags.sortAscending?"^":"v"):"" )).c_str(),left+770,top+11, uiFont(13), t.muted);
        }
        else { const std::string displayPath=currentDisplayPath(st); DrawText(displayPath.c_str(),left+14,top+11, uiFont(13), t.muted); }

        int hoverRow=-1;
        st.thumbnailProtected.clear();
        if (st.view==ViewMode::Details || st.view==ViewMode::List) {
            const int rowH=st.view==ViewMode::Details?34:30; const int visible=std::max(1,(contentH-headerH)/rowH);
            st.scroll=std::clamp(st.scroll,0,std::max(0,(int)st.visibleIndices.size()-visible));
            for(int i=0;i<visible && st.scroll+i<(int)st.visibleIndices.size();++i){
                const int vis=st.scroll+i, idx=st.visibleIndices[vis]; const VfsEntry&e=st.rows[idx]; Rectangle rr{(float)left,(float)(top+headerH+i*rowH),(float)(W-left-1),(float)rowH};
                if(pointIn(rr,mouse)){hoverRow=idx;}
                if(st.selection.count(idx)) DrawRectangleRec(rr,Fade(t.accent,0.25f)); else if(pointIn(rr,mouse)) DrawRectangleRec(rr,t.hover);
                drawEntryIcon(e,left+12,(int)rr.y+(rowH-16)/2,16,entryIconColor(e,t));
                DrawText(e.name.c_str(),left+42,(int)rr.y+9,st.view==ViewMode::Details?14:15,t.text);
                if(st.view==ViewMode::Details){ DrawText(formatTime(e.mtime).c_str(),left+500,(int)rr.y+9, uiFont(13), t.muted); DrawText(typeLabel(e).c_str(),left+700,(int)rr.y+9, uiFont(13), t.muted); if(e.kind!=EntryKind::Directory) DrawText(formatBytes(e.size).c_str(),left+770,(int)rr.y+9, uiFont(13), t.muted); }
            }
        } else {
            const int cellW=st.view==ViewMode::MediumIcons?135:180, cellH=st.view==ViewMode::MediumIcons?110:150;
            const int cols=std::max(1,(W-left-10)/cellW); const int rowsVisible=std::max(1,(contentH-headerH)/cellH); const int total=(int)st.visibleIndices.size(); const int rowCount=(total+cols-1)/cols;
            st.scroll=std::clamp(st.scroll,0,std::max(0,rowCount-rowsVisible));
            for(int r=0;r<rowsVisible;++r)for(int c=0;c<cols;++c){
                int pos=(st.scroll+r)*cols+c; if(pos>=total) break;
                int idx=st.visibleIndices[pos]; int x=left+7+c*cellW,y=top+headerH+r*cellH;
                Rectangle rr{(float)x,(float)y,(float)cellW-7,(float)cellH-7};
                if(pointIn(rr,mouse)){hoverRow=idx;}
                if(st.selection.count(idx)) DrawRectangleRounded(rr,0.06f,6,Fade(t.accent,0.25f));
                else if(pointIn(rr,mouse)) DrawRectangleRounded(rr,0.06f, uiFont(6), t.hover);
                const int icon=st.view==ViewMode::MediumIcons?48:80;
                Texture2D thumb{};
                if(st.config.showThumbnails && st.rows[idx].kind==EntryKind::File && isImagePath(st.rows[idx].name)) {
                    const std::string key = st.rows[idx].path + "#" + std::to_string(configuredThumbnailSize(st.config));
                    st.thumbnailProtected.insert(key);
                }
                if(st.config.showThumbnails && ensureThumbnail(st,st.rows[idx],configuredThumbnailSize(st.config),thumb)){
                    Rectangle td{(float)(x+(rr.width-icon)/2),(float)(y+8),(float)icon,(float)icon};
                    DrawTexturePro(thumb,{0,0,(float)thumb.width,(float)thumb.height},td,{0,0}, uiFont(0), WHITE);
                } else drawEntryIcon(st.rows[idx],x+(int)((rr.width-icon)/2),y+8,icon,entryIconColor(st.rows[idx],t));
                std::string label=st.rows[idx].name; const int maxTextW=(int)rr.width-10;
                while(label.size()>3 && MeasureText(label.c_str(),uiFont(13))>maxTextW) label=label.substr(0,label.size()-2);
                if(label!=st.rows[idx].name && label.size()>=3) label=label.substr(0,label.size()-2)+"..";
                int tw=MeasureText(label.c_str(),uiFont(13)); int tx=x+std::max(4,((int)rr.width-tw)/2);
                DrawText(label.c_str(),tx,y+icon+16, uiFont(13), t.text);
            }
        }

        if(st.selectionDragging && st.selectionDragMoved){
            Rectangle dr=normalizedRect(st.selectionDragStart,st.selectionDragCurrent);
            Rectangle clip{(float)left,(float)top,(float)(W-left),(float)contentH};
            const float x1=std::max(dr.x,clip.x), y1=std::max(dr.y,clip.y);
            const float x2=std::min(dr.x+dr.width,clip.x+clip.width), y2=std::min(dr.y+dr.height,clip.y+clip.height);
            if(x2>x1 && y2>y1){Rectangle rr{x1,y1,x2-x1,y2-y1};DrawRectangleRec(rr,Fade(t.accent,0.16f));DrawRectangleLinesEx(rr,1.0f,t.accent);}
        }

        // Status bar
        DrawRectangle(0,H-statusH,W,statusH,t.panel2); DrawLine(0,H-statusH,W,H-statusH,t.line); DrawText(st.status.c_str(), 10, H-statusH+8, uiFont(13), t.muted);

        if(st.menu.open)drawContextMenu(st,t,W,H);
        if(st.modal!=Modal::None)drawModal(st,W,H,t);
        EndDrawing();

        // ------------------------------------------------------------------
        // Mouse input
        // ------------------------------------------------------------------
        if(st.modal==Modal::ConvertImage) {
            if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) processConvertModalMouse(st,mouse);
        } else if(st.modal==Modal::CreateArchive || st.modal==Modal::ExtractArchive) {
            if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) processArchiveModalMouse(st,mouse);
        } else if(st.modal==Modal::None) {
            if(st.menu.open) {
                const int itemH=30;
                std::string info; if(st.menu.row>=0&&st.menu.row<(int)st.rows.size()&&st.vfs&&st.vfs->isLocal()) info=safeMagicOneLine(st.magic.file(st.rows[st.menu.row].path).description);
                const int menuW=300, menuH=(int)st.menu.actions.size()*itemH+10+(info.empty()?0:36);
                int x=(int)st.menu.pos.x,y=(int)st.menu.pos.y; if(x+menuW>W-4)x=W-menuW-4;if(y+menuH>H-4)y=H-menuH-4;
                const int csi = checksumSubmenuIndex(st.menu);
                const float checksumSubW=240.0f, checksumSubH=5.0f*itemH+10.0f;
                const float checksumSubX=(x+menuW+checksumSubW<=W-4)?(float)(x+menuW):(float)(x-checksumSubW);
                Rectangle checksumSub{checksumSubX, 0, checksumSubW, checksumSubH};
                bool checksumHover = false;
                if (csi >= 0) {
                    const float parentY=(float)(y+5+csi*itemH);
                    checksumSub.y=std::min(parentY,(float)H-checksumSubH-4.0f);
                    checksumHover = pointIn(Rectangle{(float)x+4,parentY,(float)menuW-8,(float)itemH},mouse) || pointIn(checksumSub,mouse);
                }
                if (csi >= 0 && checksumHover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    const int sub = (int)((mouse.y - checksumSub.y - 5) / itemH);
                    if (pointIn(checksumSub, mouse) && sub >= 0 && sub < 5) openChecksum(st, st.menu.row, sub);
                } else if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    if(!pointIn({(float)x,(float)y,(float)menuW,(float)menuH},mouse)) {
                        st.menu.open=false;
                    } else {
                        int item=(int)((mouse.y-y-5)/itemH);
                        if(item>=0&&item<(int)st.menu.actions.size()) {
                            MenuAction a=st.menu.actions[item];
                            if (a == MenuAction::Checksum) {
                                /* The adjacent submenu handles checksum choices. */
                            } else {
                                st.menu.open=false;
                                switch(a){
                                    case MenuAction::Open:openSelected(st);break;
                                    case MenuAction::OpenTerminal:
                                        if(st.menu.row>=0) openInTerminal(st); else openCurrentDirectoryInTerminal(st);
                                        break;
                                    case MenuAction::ConvertImage:openConvertImage(st);break;
                                    case MenuAction::Cat:openCat(st);break;
                                    case MenuAction::ViewImage:openImageViewer(st);break;
                                    case MenuAction::Rename:startRename(st);break;
                                    case MenuAction::Copy:doCopyOrCut(st,false);break;
                                    case MenuAction::CopyPath:
                                        if (st.menu.row >= 0 && st.menu.row < (int)st.rows.size()) SetClipboardText(st.rows[st.menu.row].path.c_str());
                                        else SetClipboardText(st.path.c_str());
                                        break;
                                    case MenuAction::Cut:doCopyOrCut(st,true);break;
                                    case MenuAction::Paste:pasteClipboard(st);break;
                                    case MenuAction::Delete:deleteSelection(st,false);break;
                                    case MenuAction::PermanentDelete:openPermanentDeleteConfirm(st);break;
                                    case MenuAction::Compress:openCreateArchive(st);break;
                                    case MenuAction::Extract:openExtractArchive(st);break;
                                    case MenuAction::OpenEditor:openInEditor(st);break;
                                    case MenuAction::Refresh:refresh(st);break;
                                    case MenuAction::NewDirectory:openNewItem(st,true);break;
                                    case MenuAction::NewFile:openNewItem(st,false);break;
                                    case MenuAction::Properties:
                                        if(st.menu.row>=0) startProperties(st); else startCurrentDirectoryProperties(st);
                                        break;
                                    default:break;
                                }
                            }
                        }
                    }
                }
            } else if(IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                openContextMenu(st,hoverRow,mouse);
                if(hoverRow>=0 && !st.selection.count(hoverRow)) selectSingle(st,hoverRow,false);
            } else if(IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
                if(mouse.y>=48 && mouse.y<78){ int ti=(int)((mouse.x-8)/170); if(ti>=0&&ti<(int)st.tabs.size()&&st.tabs.size()>1){ persistActiveTab(st); st.tabs.erase(st.tabs.begin()+ti); if(st.activeTab>=ti) st.activeTab=std::max(0,st.activeTab-1); restoreTabSnapshot(st,std::move(st.tabs[st.activeTab])); }}
            } else if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                // Tab strip / new-tab button
                if(mouse.y>=48 && mouse.y<78){
                    if(pointIn({(float)W-36,51,27,24},mouse)) newTab(st,st.path.empty()?homeDir():fs::path(st.path));
                    else {
                        int ti=(int)((mouse.x-8)/170);
                        if(ti>=0&&ti<(int)st.tabs.size()) {
                            float localX=mouse.x-(8+ti*170);
                            if(localX>140 && st.tabs.size()>1) {
                                persistActiveTab(st);
                                st.tabs.erase(st.tabs.begin()+ti);
                                if(st.activeTab>=ti) st.activeTab=std::max(0,st.activeTab-1);
                                restoreTabSnapshot(st,std::move(st.tabs[st.activeTab]));
                            } else activateTab(st,ti);
                        }
                    }
                }
                else if(pointIn(backR,mouse)){if(st.historyIndex>0){--st.historyIndex;navigate(st,st.history[st.historyIndex],false);}}
                else if(pointIn(fwdR,mouse)){if(st.historyIndex+1<(int)st.history.size()){++st.historyIndex;navigate(st,st.history[st.historyIndex],false);}}
                else if(pointIn(upR,mouse)){if(auto p=st.vfs->parent(st.path))navigate(st,*p,true);}
                else if(pointIn(addrR,mouse))focusAt(st,TextField::Address,mouse.x,addrR.x+10,14);
                else if(pointIn(searchR,mouse))focusAt(st,TextField::Search,mouse.x,searchR.x+10,14);
                else if(st.view==ViewMode::Details && mouse.y>=top && mouse.y<top+headerH){
                    SortKey k; if(mouse.x<left+500) k=SortKey::Name; else if(mouse.x<left+700) k=SortKey::Date; else if(mouse.x<left+770) k=SortKey::Type; else k=SortKey::Size;
                    if(st.sortKey==k) st.flags.sortAscending=!st.flags.sortAscending; else {st.sortKey=k;st.flags.sortAscending=true;} applySort(st); updateFilter(st);
                }
                else if(st.flags.sidebar && mouse.x<st.sidebarW && mouse.y>top){
                    int fy=top+45; bool hit=false;
                    for(const auto& f:st.config.favorites){Rectangle fr{8.0f,(float)fy-5,st.sidebarW-16.0f,28};if(pointIn(fr,mouse)){openLocalPath(st,f);hit=true;break;}fy+=29;}
                    if(!hit){
                        fy+=40;
                        Rectangle hr{8.0f,(float)fy-5,st.sidebarW-16.0f,28}; if(pointIn(hr,mouse)){openLocalPath(st,homeDir());hit=true;} fy+=29;
                        Rectangle rr{8.0f,(float)fy-5,st.sidebarW-16.0f,28}; if(pointIn(rr,mouse)){openLocalPath(st,"/");hit=true;} fy+=29;
                        Rectangle tr{8.0f,(float)fy-5,st.sidebarW-16.0f,28}; if(pointIn(tr,mouse)){openLocalPath(st,"/tmp");hit=true;} fy+=29;
                        fy+=40; Rectangle diskR{8.0f,(float)fy-5,st.sidebarW-16.0f,28}; if(pointIn(diskR,mouse)){openDiskInfo(st);hit=true;} fy+=29; fy+=40;
                        Rectangle xr{8.0f,(float)fy-5,st.sidebarW-16.0f,28}; if(pointIn(xr,mouse)){openLocalPath(st,xdgTrashHome()/"files");hit=true;} fy+=29;
                        Rectangle dr{8.0f,(float)fy-5,st.sidebarW-16.0f,28}; if(pointIn(dr,mouse)){openLocalPath(st,xdgDataHome());hit=true;} fy+=29;
                        Rectangle cr{8.0f,(float)fy-5,st.sidebarW-16.0f,28}; if(pointIn(cr,mouse)){openLocalPath(st,configBase());hit=true;}
                    }
                    if(!hit){st.selection.clear();unfocus(st);}
                } else if(pointIn(listR,mouse)){
                    st.selectionDragging=true;
                    st.selectionDragMoved=false;
                    st.selectionDragStart=mouse;
                    st.selectionDragCurrent=mouse;
                    st.selectionDragAdditive=isCtrlDown();
                    st.selectionDragBase=st.selectionDragAdditive?st.selection:std::set<int>{};
                    if(hoverRow>=0){
                        if(isShiftDown()) selectRange(st,hoverRow);
                        else selectSingle(st,hoverRow,isCtrlDown());
                    } else if(!isCtrlDown()){
                        st.selection.clear();
                        st.selected=-1;
                        st.anchorSelection=-1;
                    }
                    unfocus(st);
                } else {st.selection.clear();unfocus(st);}
            }
        }

        if(st.modal==Modal::None && st.selectionDragging){
            if(IsMouseButtonDown(MOUSE_BUTTON_LEFT)){
                const Vector2 m=GetMousePosition();
                if(std::fabs(m.x-st.selectionDragStart.x)>4.0f || std::fabs(m.y-st.selectionDragStart.y)>4.0f) st.selectionDragMoved=true;
                st.selectionDragCurrent=m;
                if(st.selectionDragMoved) updateDragSelection(st,left,top,contentH,W);
            }
            if(IsMouseButtonReleased(MOUSE_BUTTON_LEFT)){
                const bool moved=st.selectionDragMoved; st.selectionDragging=false; st.selectionDragMoved=false;
                if(moved) updateDragSelection(st,left,top,contentH,W);
                else if(hoverRow>=0){
                    static double lastClickTime=0.0; static int lastClickRow=-1; const double now=GetTime();
                    if(lastClickRow==hoverRow && now-lastClickTime<0.35) openSelected(st);
                    lastClickRow=hoverRow; lastClickTime=now;
                }
            }
        }

        if(st.modal==Modal::ThemePicker && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Rectangle box{W*0.5f-290,H*0.5f-220,580,440};
            Rectangle themeF{box.x+20,box.y+92,150,34}, editorF{box.x+20,box.y+152,270,34}, termF{box.x+20,box.y+212,270,34}, fontF{box.x+20,box.y+272,170,34}, accentF{box.x+200,box.y+272,170,34}, hsvBtn{box.x+380,box.y+272,62,34};
            Rectangle apply{box.x+390,box.y+360,85,32}, cancel{box.x+485,box.y+360,65,32};
            if(pointIn(themeF,mouse)) focusAt(st,TextField::ThemeNumber,mouse.x,themeF.x+8,13);
            else if(pointIn(editorF,mouse)) focusAt(st,TextField::EditorConfig,mouse.x,editorF.x+8,13);
            else if(pointIn(termF,mouse)) focusAt(st,TextField::TermConfig,mouse.x,termF.x+8,13);
            else if(pointIn(fontF,mouse)) focusAt(st,TextField::FontScale,mouse.x,fontF.x+8,13);
            else if(pointIn(accentF,mouse)) focusAt(st,TextField::AccentConfig,mouse.x,accentF.x+8,13);
            else if(pointIn(hsvBtn,mouse)){ st.config.accent=ColorFromHSV(std::fmod((float)(GetTime()*53.0),360.0f),0.72f,0.96f); st.config.accentCustom=true; st.accentEdit=hexRGB(st.config.accent); st.flags.configDirty=true; }
            else if(pointIn(apply,mouse)) {
                int n=std::atoi(st.themeEdit.c_str()); if(n>=0&&n<(int)kThemes.size()) st.config.theme=n;
                st.config.editor=st.editorEdit; st.config.term=st.termEdit; st.config.fontScale=std::clamp(std::strtof(st.fontScaleEdit.c_str(),nullptr),1.0f,2.5f); gUIFontScale=st.config.fontScale;
                st.config.accent=accentFromHSVField(st.accentEdit, st.config.accent); st.config.accentCustom=true; st.flags.configDirty=true; st.modal=Modal::None; unfocus(st);
            } else if(pointIn(cancel,mouse)){ st.modal=Modal::None; unfocus(st); }
            else { int yy=(int)box.y+100; for(int i=0;i<(int)kThemes.size();++i){ Rectangle r{box.x+330,(float)yy-3,230,18}; if(pointIn(r,mouse)){ st.themeEdit=std::to_string(i); st.config.theme=i; if(!st.config.accentCustom) st.config.accent=kThemes[i].accent; st.flags.configDirty=true; } yy+=17; } }
        }
        if(st.modal==Modal::DiskInfo && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
            Rectangle box{W*0.5f-290,H*0.5f-220,580,440}; const int n=(int)st.diskInfo.mounts.size();
            const int tabLeft=(int)box.x+52, tabRight=(int)box.x+(int)box.width-52, tabAreaW=tabRight-tabLeft, tabW=132, visibleTabs=std::max(1,tabAreaW/tabW);
            Rectangle leftTab{box.x+20,box.y+18,26,28}, rightTab{box.x+box.width-46,box.y+18,26,28};
            if(pointIn(leftTab,mouse) && st.diskInfo.firstTab>0) --st.diskInfo.firstTab;
            else if(pointIn(rightTab,mouse) && st.diskInfo.firstTab+visibleTabs<n) ++st.diskInfo.firstTab;
            else {
                for(int slot=0;slot<visibleTabs && st.diskInfo.firstTab+slot<n;++slot){ Rectangle tr{(float)(tabLeft+slot*tabW),(float)box.y+18,(float)tabW-4,28}; if(pointIn(tr,mouse)){st.diskInfo.tab=st.diskInfo.firstTab+slot;break;} }
            }
            Rectangle closeR{box.x+440,box.y+344,70,32}; if(pointIn(closeR,mouse)){st.modal=Modal::None;unfocus(st);}
        }
        if(st.modal==Modal::DiskInfo && IsKeyPressed(KEY_LEFT) && !st.diskInfo.mounts.empty()){ st.diskInfo.tab=std::max(0,st.diskInfo.tab-1); }
        if(st.modal==Modal::DiskInfo && IsKeyPressed(KEY_RIGHT) && !st.diskInfo.mounts.empty()){ st.diskInfo.tab=std::min((int)st.diskInfo.mounts.size()-1,st.diskInfo.tab+1); }
        if(st.modal==Modal::DiskInfo && IsKeyPressed(KEY_PAGE_UP) && !st.diskInfo.mounts.empty()){ st.diskInfo.tab=std::max(0,st.diskInfo.tab-3); }
        if(st.modal==Modal::DiskInfo && IsKeyPressed(KEY_PAGE_DOWN) && !st.diskInfo.mounts.empty()){ st.diskInfo.tab=std::min((int)st.diskInfo.mounts.size()-1,st.diskInfo.tab+3); }
        if(st.modal==Modal::Checksum && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
            Rectangle box{W*0.5f-290,H*0.5f-220,580,440};
            Rectangle hashR{box.x+20, box.y+172, box.width-40, 42};
            Rectangle closeR{box.x+440,box.y+344,90,32};
            if(pointIn(hashR,mouse) && st.checksumValue.rfind("ERROR:",0) != 0 && st.checksumValue != "Calculating...") { SetClipboardText(st.checksumValue.c_str()); st.status="Checksum copied to clipboard"; }
            else if(pointIn(closeR,mouse)){ st.modal=Modal::None; unfocus(st); }
        }
        if(st.modal==Modal::ConfirmPermanentDelete && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
            Rectangle box{W*0.5f-290,H*0.5f-220,580,440};
            Rectangle apply{box.x+350,box.y+180,110,34}, cancel{box.x+470,box.y+180,70,34};
            if(pointIn(apply,mouse)) applyPermanentDelete(st);
            else if(pointIn(cancel,mouse)){ st.pendingPermanentDelete.clear(); st.modal=Modal::None; unfocus(st); }
        }
        if(st.modal==Modal::PasteOverwrite && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
            Rectangle box{W*0.5f-290,H*0.5f-220,580,440}; Rectangle apply{box.x+360,box.y+180,100,32}, cancel{box.x+470,box.y+180,70,32};
            if(pointIn(apply,mouse)){
                if (finishPasteOne(st, st.pendingPasteSource, st.pendingPasteDestination, st.flags.pendingPasteCut, true)) {
                    ++st.pendingPasteIndex;
                    st.modal = Modal::None;
                    unfocus(st);
                    continuePasteClipboard(st);
                }
            }
            else if(pointIn(cancel,mouse)){ st.modal=Modal::None; unfocus(st); }
        }
        if(st.modal==Modal::Properties && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Rectangle box{W*0.5f-290,H*0.5f-220,580,440};
            if (st.props.multi) {
                Rectangle closeR{box.x+440, box.y+210, 70, 32};
                if (pointIn(closeR, mouse)) { st.modal=Modal::None; unfocus(st); }
            } else {
            // Mode editor field
            Rectangle modeR{box.x+68,box.y+217,90,31};
            if(pointIn(modeR,mouse)) focus(st,TextField::Mode,false);
            for(int c=0;c<3;++c)for(int r=0;r<3;++r){Rectangle cb{box.x+25+c*170+r*44,box.y+290,16,16};if(pointIn(cb,mouse)){modeToBits(st.props);toggleModeBit(st.props,c,r);}}
            Rectangle saveR{box.x+440,box.y+364,70,32}, cancelR{box.x+515,box.y+364,45,32};
            if(pointIn(saveR,mouse)) { saveProperties(st); }
            if(pointIn(cancelR,mouse)) { st.modal=Modal::None; unfocus(st); }
            }
        }
        if(st.modal==Modal::NewItem && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
            Rectangle box{W*0.5f-290,H*0.5f-220,580,440},saveR{box.x+390,box.y+105,85,32},cancelR{box.x+485,box.y+105,65,32};
            if(pointIn(saveR,mouse)) { createNewItem(st); }
            if(pointIn(cancelR,mouse)) { st.modal=Modal::None; unfocus(st); }
            Rectangle field{box.x+20,box.y+95,box.width-240,38};
            if(pointIn(field,mouse)) focusAt(st,TextField::NewItem,mouse.x,field.x+8,14);
        }
        if(st.modal==Modal::Rename && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){
            Rectangle box{W*0.5f-290,H*0.5f-220,580,440},saveR{box.x+390,box.y+105,85,32},cancelR{box.x+485,box.y+105,65,32};
            if(pointIn(saveR,mouse)) { applyRename(st); }
            if(pointIn(cancelR,mouse)) { st.modal=Modal::None; unfocus(st); }
        }
    }
    persistActiveTab(st);
    if (st.flags.configDirty) saveConfig(st.config);
    for(auto& kv:st.thumbnailCache) if(kv.second.id) UnloadTexture(kv.second);
    st.thumbnailCache.clear();
    st.thumbnailUse.clear();
    if(st.imageTexture.id) UnloadTexture(st.imageTexture);
    thumbnailWorker.stop();
    gThumbnailWorker = nullptr;
    vips_shutdown();
    CloseWindow();
    return 0;
}
