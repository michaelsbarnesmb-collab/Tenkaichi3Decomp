/* In-game auto-updater: downloads release zips from GitHub and updates binaries cleanly. */
#include "net_update.h"
#include "net_match.h"
#include "../port_release.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../../third_party/miniz/miniz.h"
#include "../../third_party/miniz/miniz.c"

static SDL_AtomicInt sStatus = {PORT_UPDATE_IDLE};
static SDL_SpinLock sLock;
static size_t sDownloadedBytes = 0;
static size_t sTotalBytes = 0;
static char sErrorMsg[256] = "";

static void set_error(const char *msg) {
    SDL_LockSpinlock(&sLock);
    SDL_strlcpy(sErrorMsg, msg, sizeof(sErrorMsg));
    SDL_UnlockSpinlock(&sLock);
    SDL_SetAtomicInt(&sStatus, PORT_UPDATE_ERROR);
}

PortUpdateStatus PortUpdate_GetStatus(void) {
    return (PortUpdateStatus)SDL_GetAtomicInt(&sStatus);
}

float PortUpdate_GetProgress(void) {
    float p = 0.0f;
    SDL_LockSpinlock(&sLock);
    if (sTotalBytes > 0) {
        p = (float)sDownloadedBytes / (float)sTotalBytes;
        if (p > 1.0f) p = 1.0f;
    }
    SDL_UnlockSpinlock(&sLock);
    return p;
}

void PortUpdate_GetBytes(size_t *downloaded, size_t *total) {
    SDL_LockSpinlock(&sLock);
    if (downloaded) *downloaded = sDownloadedBytes;
    if (total) *total = sTotalBytes;
    SDL_UnlockSpinlock(&sLock);
}

const char *PortUpdate_GetError(void) {
    return sErrorMsg;
}

#ifdef _WIN32
static void safe_swap(const char *name) {
    char oldName[512], newName[512];
    FILE *f;
    SDL_snprintf(oldName, sizeof(oldName), "%s.old", name);
    SDL_snprintf(newName, sizeof(newName), "%s.new", name);

    f = fopen(newName, "rb");
    if (f == NULL) {
        return;
    }
    fclose(f);

    remove(oldName);
    rename(name, oldName);
    rename(newName, name);
}

static int download_winhttp(const char *url, const char *destFile) {
    wchar_t wurl[1024], host[256], path[768];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD got = 0, status = 0, statusLen = sizeof(status), contentLen = 0, clen = sizeof(contentLen);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    FILE *fp = NULL;
    char buf[65536];
    int ok = 0;

    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024) == 0) {
        return 0;
    }
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 768;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        return 0;
    }

    ses = WinHttpOpen(L"Tenkaichi3Decomp-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses != NULL) {
        WinHttpSetTimeouts(ses, 10000, 10000, 10000, 30000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con != NULL) {
        req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    }
    if (req != NULL) {
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
    }
    if (req != NULL && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &contentLen, &clen, WINHTTP_NO_HEADER_INDEX)) {
                SDL_LockSpinlock(&sLock);
                sTotalBytes = (size_t)contentLen;
                SDL_UnlockSpinlock(&sLock);
            }
            fp = fopen(destFile, "wb");
            if (fp != NULL) {
                while (WinHttpReadData(req, buf, sizeof(buf), &got) && got > 0) {
                    fwrite(buf, 1, got, fp);
                    SDL_LockSpinlock(&sLock);
                    sDownloadedBytes += got;
                    SDL_UnlockSpinlock(&sLock);
                }
                fclose(fp);
                ok = (sDownloadedBytes > 0);
            }
        }
    }
    if (req != NULL) WinHttpCloseHandle(req);
    if (con != NULL) WinHttpCloseHandle(con);
    if (ses != NULL) WinHttpCloseHandle(ses);
    return ok;
}
#endif

static int download_curl_fallback(const char *url, const char *destFile) {
#ifdef _WIN32
    const char *args[] = {"curl.exe", "-s", "-L", "-o", destFile, url, NULL};
#else
    const char *args[] = {"curl", "-s", "-L", "-o", destFile, url, NULL};
#endif
    SDL_Process *pr = SDL_CreateProcess(args, false);
    if (pr == NULL) {
        return 0;
    }
    while (1) {
        FILE *f = fopen(destFile, "rb");
        if (f != NULL) {
            fseek(f, 0, SEEK_END);
            SDL_LockSpinlock(&sLock);
            sDownloadedBytes = (size_t)ftell(f);
            SDL_UnlockSpinlock(&sLock);
            fclose(f);
        }
        if (SDL_WaitProcess(pr, false, NULL)) {
            break;
        }
        SDL_Delay(100);
    }
    SDL_DestroyProcess(pr);
    return sDownloadedBytes > 1000;
}

static int SDLCALL update_worker(void *arg) {
    int target = (int)(intptr_t)arg;
    char url[512];
    const char *tempZip = "update_temp.zip";
    int ok = 0;

    SDL_LockSpinlock(&sLock);
    sDownloadedBytes = 0;
    sTotalBytes = 0;
    sErrorMsg[0] = '\0';
    SDL_UnlockSpinlock(&sLock);

    const char *tag = Match_UpdateTag();
    if (tag != NULL && tag[0] != '\0') {
#ifdef _WIN32
        SDL_snprintf(url, sizeof(url), "https://github.com/" PORT_UPDATE_REPO "/releases/download/%s/Tenkaichi3Decomp-windows-x64.zip", tag);
#else
        SDL_snprintf(url, sizeof(url), "https://github.com/" PORT_UPDATE_REPO "/releases/download/%s/Tenkaichi3Decomp-linux-x64.zip", tag);
#endif
    } else {
#ifdef _WIN32
        SDL_snprintf(url, sizeof(url), "https://github.com/" PORT_UPDATE_REPO "/releases/latest/download/Tenkaichi3Decomp-windows-x64.zip");
#else
        SDL_snprintf(url, sizeof(url), "https://github.com/" PORT_UPDATE_REPO "/releases/latest/download/Tenkaichi3Decomp-linux-x64.zip");
#endif
    }

#ifdef _WIN32
    ok = download_winhttp(url, tempZip);
    if (!ok && tag != NULL && tag[0] != '\0') {
        SDL_snprintf(url, sizeof(url), "https://github.com/" PORT_UPDATE_REPO "/releases/latest/download/Tenkaichi3Decomp-windows-x64.zip");
        ok = download_winhttp(url, tempZip);
    }
#endif

    if (!ok) {
        ok = download_curl_fallback(url, tempZip);
    }

    if (!ok) {
        remove(tempZip);
        set_error("Download failed. Check your internet connection or open download page.");
        return 0;
    }

    SDL_SetAtomicInt(&sStatus, PORT_UPDATE_EXTRACTING);

    /* Extract using embedded miniz */
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, tempZip, 0)) {
        remove(tempZip);
        set_error("Failed to open downloaded archive.");
        return 0;
    }

    mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || st.m_is_directory) {
            continue;
        }
        const char *fn = st.m_filename;
        if (strncmp(fn, "Tenkaichi3Decomp/", 17) == 0) {
            fn += 17;
        }
        if (*fn == '\0') {
            continue;
        }

        const char *slash = strrchr(fn, '/');
        if (slash != NULL) {
            char dir[256];
            size_t dlen = slash - fn;
            if (dlen < sizeof(dir)) {
                memcpy(dir, fn, dlen);
                dir[dlen] = '\0';
#ifdef _WIN32
                CreateDirectoryA(dir, NULL);
#else
                mkdir(dir, 0755);
#endif
            }
        }

        char outPath[512];
        if (strcmp(fn, "Tenkaichi3Decomp.exe") == 0 ||
            strcmp(fn, "Tenkaichi3Decomp.dat") == 0 ||
            strcmp(fn, "Tenkaichi3Decomp.exe.mem") == 0 ||
            strcmp(fn, "Tenkaichi3Decomp-setup.exe") == 0 ||
            strcmp(fn, "SDL3.dll") == 0 ||
            strcmp(fn, "Tenkaichi3Decomp") == 0 ||
            strcmp(fn, "Tenkaichi3Decomp-setup") == 0) {
            SDL_snprintf(outPath, sizeof(outPath), "%s.new", fn);
        } else {
            SDL_snprintf(outPath, sizeof(outPath), "%s", fn);
        }

        remove(outPath);
        mz_zip_reader_extract_to_file(&zip, i, outPath, 0);
    }
    mz_zip_reader_end(&zip);
    remove(tempZip);

#ifdef _WIN32
    safe_swap("Tenkaichi3Decomp.exe");
    safe_swap("Tenkaichi3Decomp.dat");
    safe_swap("Tenkaichi3Decomp.exe.mem");
    safe_swap("Tenkaichi3Decomp-setup.exe");
    safe_swap("SDL3.dll");
#else
    remove("Tenkaichi3Decomp.old");
    rename("Tenkaichi3Decomp", "Tenkaichi3Decomp.old");
    rename("Tenkaichi3Decomp.new", "Tenkaichi3Decomp");
    rename("Tenkaichi3Decomp.dat.new", "Tenkaichi3Decomp.dat");
    rename("Tenkaichi3Decomp.mem.new", "Tenkaichi3Decomp.mem");
    rename("Tenkaichi3Decomp-setup.new", "Tenkaichi3Decomp-setup");
    chmod("Tenkaichi3Decomp", 0755);
    chmod("Tenkaichi3Decomp-setup", 0755);
#endif

    SDL_SetAtomicInt(&sStatus, PORT_UPDATE_READY);
    return 0;
}

void PortUpdate_Start(int targetRelease) {
    int st = SDL_GetAtomicInt(&sStatus);
    if (st == PORT_UPDATE_DOWNLOADING || st == PORT_UPDATE_EXTRACTING) {
        return;
    }
    SDL_SetAtomicInt(&sStatus, PORT_UPDATE_DOWNLOADING);
    SDL_Thread *t = SDL_CreateThread(update_worker, "bt3-updater", (void *)(intptr_t)targetRelease);
    if (t != NULL) {
        SDL_DetachThread(t);
    } else {
        set_error("Could not start updater thread.");
    }
}

void PortUpdate_Restart(void) {
#ifdef _WIN32
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessA("Tenkaichi3Decomp.exe", NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    exit(0);
#else
    const char *args[] = {"./Tenkaichi3Decomp", NULL};
    execv(args[0], (char *const *)args);
    exit(0);
#endif
}

void PortUpdate_Cleanup(void) {
    remove("Tenkaichi3Decomp.exe.old");
    remove("Tenkaichi3Decomp.dat.old");
    remove("Tenkaichi3Decomp.exe.mem.old");
    remove("Tenkaichi3Decomp-setup.exe.old");
    remove("SDL3.dll.old");
    remove("update_temp.zip");
}
