// The launcher side of fut_game; see Launcher.h.
#include "platform/sdl/Launcher.h"

#include "platform/sdl/Window.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <fstream>
#ifdef __ANDROID__
#include <jni.h>
#endif
#include <mutex>
#include <vector>

namespace fut::platform {
namespace {
namespace sfs = std::filesystem;

std::string Utf8(const sfs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

sfs::path PathOf(const std::string& utf8) { return sfs::path(std::u8string(utf8.begin(), utf8.end())); }

#ifdef __ANDROID__
// A content:// document (the system file picker's answer, or one remembered from it) as "fd:<n>" for Xdvdfs: the
// app has no storage permission, so a picked .iso in Download etc. can only be read through the ContentResolver.
// persist: keep the read grant across launches (the picker's OPEN_DOCUMENT grants are persistable).
std::string OpenContentUri(const std::string& uri, bool persist) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !act) return {};
    std::string out;
    auto clear = [&] {
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return true;
        }
        return false;
    };
    jclass actCls = env->GetObjectClass(act);
    jobject cr = env->CallObjectMethod(act, env->GetMethodID(actCls, "getContentResolver",
                                                             "()Landroid/content/ContentResolver;"));
    jclass uriCls = env->FindClass("android/net/Uri");
    jstring juri = env->NewStringUTF(uri.c_str());
    jobject u = env->CallStaticObjectMethod(uriCls, env->GetStaticMethodID(uriCls, "parse",
                                                                           "(Ljava/lang/String;)Landroid/net/Uri;"),
                                            juri);
    if (!clear() && cr && u) {
        jclass crCls = env->GetObjectClass(cr);
        if (persist) {
            env->CallVoidMethod(cr, env->GetMethodID(crCls, "takePersistableUriPermission", "(Landroid/net/Uri;I)V"),
                                u, jint(1) /* FLAG_GRANT_READ_URI_PERMISSION */);
            if (clear()) std::fprintf(stderr, "launcher: no persistable grant for %s\n", uri.c_str());
        }
        jstring mode = env->NewStringUTF("r");
        jobject pfd = env->CallObjectMethod(cr, env->GetMethodID(crCls, "openFileDescriptor",
                                                                 "(Landroid/net/Uri;Ljava/lang/String;)"
                                                                 "Landroid/os/ParcelFileDescriptor;"),
                                            u, mode);
        if (!clear() && pfd) {
            jclass pfdCls = env->GetObjectClass(pfd);
            const jint fd = env->CallIntMethod(pfd, env->GetMethodID(pfdCls, "detachFd", "()I"));
            if (!clear() && fd >= 0) out = "fd:" + std::to_string(fd);
            env->DeleteLocalRef(pfdCls);
            env->DeleteLocalRef(pfd);
        }
        env->DeleteLocalRef(mode);
        env->DeleteLocalRef(crCls);
    }
    if (u) env->DeleteLocalRef(u);
    env->DeleteLocalRef(juri);
    env->DeleteLocalRef(uriCls);
    if (cr) env->DeleteLocalRef(cr);
    env->DeleteLocalRef(actCls);
    env->DeleteLocalRef(act);
    return out;
}
bool IsContentUri(const std::string& s) { return s.rfind("content://", 0) == 0; }

// The launch intent's data URI, when the game was opened on a file ("Open with" from a file manager, or
// am start -d <uri> --grant-read-uri-permission). Empty otherwise.
std::string LaunchIntentUri() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !act) return {};
    std::string out;
    jclass actCls = env->GetObjectClass(act);
    jobject intent = env->CallObjectMethod(act, env->GetMethodID(actCls, "getIntent", "()Landroid/content/Intent;"));
    if (!env->ExceptionCheck() && intent) {
        jclass intentCls = env->GetObjectClass(intent);
        jstring ds = static_cast<jstring>(
            env->CallObjectMethod(intent, env->GetMethodID(intentCls, "getDataString", "()Ljava/lang/String;")));
        if (!env->ExceptionCheck() && ds) {
            if (const char* c = env->GetStringUTFChars(ds, nullptr)) {
                out = c;
                env->ReleaseStringUTFChars(ds, c);
            }
            env->DeleteLocalRef(ds);
        }
        env->DeleteLocalRef(intentCls);
        env->DeleteLocalRef(intent);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(actCls);
    env->DeleteLocalRef(act);
    return out;
}
#endif

// The path as the narrow string Vfs::Open takes (the C library's narrow file names: on Windows the ANSI code page,
// as a command-line argument; a path with characters outside it goes by its short name; "" when neither works).
// Elsewhere file names are UTF-8 already.
std::string Narrow(const sfs::path& p) {
#ifdef _WIN32
    auto acp = [](const std::wstring& w, std::string& out) {
        BOOL lossy = FALSE;
        const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), nullptr,
                                          0, nullptr, &lossy);
        if (n <= 0 || lossy) return false;
        out.assign(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                            nullptr);
        return true;
    };
    std::string s;
    if (acp(p.wstring(), s)) return s;
    std::wstring shortName(32768, L'\0');
    const DWORD n = GetShortPathNameW(p.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    if (n == 0 || n >= shortName.size()) return {};
    shortName.resize(n);
    return acp(shortName, s) ? s : std::string();
#else
    return Utf8(p);
#endif
}

sfs::path SettingsFile() { return PrefFolder() / "settings.ini"; }

sfs::path Remembered(const sfs::path& settings) {
    std::ifstream in(settings, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("disc=", 0) == 0) return PathOf(line.substr(5));
    }
    return {};
}

void Remember(const sfs::path& settings, const sfs::path& disc) {
    if (settings.empty()) return;
    std::error_code ec;
    sfs::create_directories(settings.parent_path(), ec);
    std::ofstream out(settings, std::ios::binary | std::ios::trunc);
    out << "disc=" << Utf8(disc) << "\r\n";
}

sfs::path ExeFolder() {
    const char* base = SDL_GetBasePath();             // (the exe's folder, UTF-8, with a trailing separator)
    return base ? PathOf(base) : sfs::current_path();
}

bool HasXbe(const sfs::path& dir) {
    std::error_code ec;
    return sfs::is_regular_file(dir / "default.xbe", ec);
}

bool NameIs(const sfs::path& p, const char* name) {
    std::string n = Utf8(p);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n == name;
}

// Steps 2 and 3 of the search (Launcher.h), in order.
std::vector<sfs::path> NextToExe(const sfs::path& dir) {
    std::vector<sfs::path> isos, folders;
    std::error_code ec;
    for (auto it = sfs::directory_iterator(dir, ec); !ec && it != sfs::directory_iterator(); it.increment(ec)) {
        const sfs::path& p = it->path();
        if (it->is_regular_file(ec) && NameIs(p.extension(), ".iso")) isos.push_back(p);
        else if (it->is_directory(ec) && !NameIs(p.filename(), "futurama") && HasXbe(p)) folders.push_back(p);
    }
    std::vector<sfs::path> out;
    if (isos.size() == 1) out.push_back(isos[0]);
    if (HasXbe(dir / "futurama")) out.push_back(dir / "futurama");
    if (!folders.empty()) out.push_back(folders[0]);
    if (HasXbe(dir)) out.push_back(dir);
    return out;
}

struct DialogResult {
    std::mutex m;
    bool done = false;
    std::string path;                                 // UTF-8; empty = cancelled or failed
};

void SDLCALL OnDiscPicked(void* userdata, const char* const* files, int) {   // (may run on another thread)
    auto* r = static_cast<DialogResult*>(userdata);
    std::lock_guard g(r->m);
    if (files && files[0]) r->path = files[0];
    r->done = true;
}

// The file-open dialog: an .iso, or the default.xbe of an extracted folder (the folder is the source). Empty when
// cancelled. (SDL's dialog is asynchronous: the events are pumped until its callback has run.)
sfs::path AskDisc(const sfs::path& dir) {
    Window::InitSdl();                                // (the dialog needs the video subsystem)
    static const SDL_DialogFileFilter kFilters[] = {{"Futurama (Xbox) disc image (*.iso)", "iso"},
                                                    {"Extracted disc folder (its default.xbe)", "xbe"},
                                                    {"All files", "*"}};
    DialogResult r;
    const std::string initial = Utf8(dir);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(kFilters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 3);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, initial.c_str());
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Select your Futurama (Xbox) disc image");
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, OnDiscPicked, &r, props);
    SDL_DestroyProperties(props);
    for (;;) {
        SDL_PumpEvents();
        {
            std::lock_guard g(r.m);
            if (r.done) break;
        }
        SDL_Delay(10);
    }
    if (r.path.empty()) return {};
    sfs::path p = PathOf(r.path);
    if (NameIs(p.filename(), "default.xbe")) p = p.parent_path();
    return p;
}

}  // namespace

bool SetUpConsole() {
#ifdef _WIN32
    if (GetConsoleWindow()) return false;              // (the console subsystem, or a console of its own)
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if ((err == nullptr || err == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    return true;
#else
    return false;
#endif
}

void ErrorBox(const std::string& text) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Futurama", text.c_str(), nullptr);
}

std::filesystem::path PrefFolder() {
    char* p = SDL_GetPrefPath("", "FuturamaRecomp");  // (%APPDATA%\FuturamaRecomp\ on Windows)
    if (!p) return sfs::path("FuturamaRecomp");
    const sfs::path folder = PathOf(p);
    SDL_free(p);
    return folder;
}

std::string FindDisc(const std::function<bool(const std::string& source, std::string& error)>& check) {
    std::string error;
    auto test = [&](const sfs::path& p, std::string& source) {
        source = Narrow(p);
        if (source.empty()) {
            error = "the path has characters that cannot be opened here (try a folder with a plain name)";
            return false;
        }
#ifdef __ANDROID__
        if (IsContentUri(source)) {                   // (picked with the system file picker)
            const std::string fdPath = OpenContentUri(source, true);
            if (fdPath.empty()) {
                error = "the picked file can't be opened (pick it again)";
                return false;
            }
            source = fdPath;
        }
#endif
        return check(source, error);
    };
    std::string source;
    const sfs::path settings = SettingsFile();
    std::error_code ec;
#ifdef __ANDROID__
    if (const std::string uri = LaunchIntentUri(); IsContentUri(uri)) {   // (opened on a file)
        if (test(PathOf(uri), source)) {
            Remember(settings, PathOf(uri));
            return source;
        }
        ErrorBox("The file the game was opened with can't be used:\n\n" + error);
    }
#endif
    if (const sfs::path p = Remembered(settings); !p.empty() &&
#ifdef __ANDROID__
                                                   (IsContentUri(Utf8(p)) || sfs::exists(p, ec)) &&
#else
                                                   sfs::exists(p, ec) &&
#endif
                                                   test(p, source))
        return source;
    const sfs::path dir = ExeFolder();
    for (const sfs::path& p : NextToExe(dir)) {
        if (test(p, source)) return source;
        ErrorBox("Found " + Utf8(p) + " next to the game, but it can't be used:\n\n" + error);
    }
#ifdef __ANDROID__
    {
        std::vector<sfs::path> dirs;
        std::string extHint;
        if (const char* ext = SDL_GetAndroidExternalStoragePath()) {
            extHint = ext;
            dirs.push_back(PathOf(ext));
        }
        dirs.push_back(PrefFolder());
        dirs.push_back(sfs::path("/sdcard/Futurama"));
        dirs.push_back(sfs::path("/sdcard/Download"));
        for (const sfs::path& d : dirs) {
            if (d.empty() || !sfs::exists(d, ec)) continue;
            for (const sfs::path& p : NextToExe(d)) {
                if (test(p, source)) {
                    Remember(settings, p);
                    return source;
                }
            }
            for (auto it = sfs::directory_iterator(d, ec); !ec && it != sfs::directory_iterator(); it.increment(ec)) {
                const sfs::path& p = it->path();
                if (!it->is_regular_file(ec) || !NameIs(p.extension(), ".iso")) continue;
                std::string fn = Utf8(p.filename());
                for (char& c : fn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (fn.find("futurama") != std::string::npos && test(p, source)) {
                    Remember(settings, p);
                    return source;
                }
            }
        }
        const sfs::path p = AskDisc(dirs.empty() ? dir : dirs[0]);
        if (!p.empty() && test(p, source)) {
            Remember(settings, p);
            return source;
        }
        ErrorBox("No Futurama (Xbox) disc image (.iso) picked.\n\nStart the game again and pick your Futurama .iso "
                 "(for example from Download), or copy it to:\n" +
                 (extHint.empty() ? std::string("/sdcard/Android/data/com.futuramarecomp.app/files/") : extHint) +
                 (error.empty() ? std::string() : ("\n\nLast error: " + error)));
        return {};
    }
#endif
    for (;;) {
        const sfs::path p = AskDisc(dir);
        if (p.empty()) return {};
        if (test(p, source)) {
            Remember(settings, p);
            return source;
        }
        const std::string text = "That one can't be used:\n\n" + error + "\n\nThe game needs your own Futurama (Xbox, "
                                 "NTSC-U or PAL) disc image (.iso), or the default.xbe of the extracted disc.";
        const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
                                                    {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Pick another"}};
        const SDL_MessageBoxData box = {SDL_MESSAGEBOX_WARNING, nullptr, "Futurama", text.c_str(), 2, buttons, nullptr};
        int pressed = 0;
        if (!SDL_ShowMessageBox(&box, &pressed) || pressed != 1) return {};
    }
}

}  // namespace fut::platform
