#include "util/download.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

#include "util/sha256.h"

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace wisp {

#ifndef __ANDROID__
DownloadResult downloadFile(const std::string& url, const std::string& path, const std::string&, std::atomic<long long>* bytes) {
    DownloadResult r;
    std::string cmd = "curl -fL --retry 3 -sS '" + url + "'";  // body on stdout, errors on stderr
    FILE* in = popen(cmd.c_str(), "r");
    if (!in) { r.err = "cannot run curl"; return r; }
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) { pclose(in); r.err = "cannot write " + path; return r; }
    Sha256 h;
    std::vector<char> buf(1 << 16);
    long long got = 0;
    size_t n;
    bool wfail = false;
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
        h.update(buf.data(), n);
        if (std::fwrite(buf.data(), 1, n, out) != n) { wfail = true; break; }
        got += (long long)n;
        if (bytes) *bytes = got;
    }
    std::fclose(out);
    int rc = pclose(in);
    if (wfail) { r.err = "write failed (disk full?)"; return r; }
    if (rc != 0) { r.err = "download failed (curl exit " + std::to_string(rc) + ")"; return r; }
    r.sha256 = h.hexdigest();
    r.ok = true;
    return r;
}
#else
// JNI glue: static methods on the app's activity class (found through the activity object, so the app class loader
// is used even from this worker thread; SDL_GetAndroidJNIEnv attaches the thread).
namespace {
struct Jni {
    JNIEnv* env = nullptr;
    jclass cls = nullptr;
    bool ok() const { return env && cls; }
};
Jni jni() {
    Jni j;
    j.env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!j.env) return j;
    jobject act = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!act) return j;
    j.cls = j.env->GetObjectClass(act);
    j.env->DeleteLocalRef(act);
    return j;
}
bool clearExc(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}
}  // namespace

DownloadResult downloadFile(const std::string& url, const std::string& path, const std::string& title, std::atomic<long long>* bytes) {
    DownloadResult r;
    Jni j = jni();
    if (!j.ok()) { r.err = "no JNI environment"; return r; }
    JNIEnv* env = j.env;
    jmethodID mStart = env->GetStaticMethodID(j.cls, "startDownload", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J");
    jmethodID mQuery = env->GetStaticMethodID(j.cls, "queryDownload", "(J)[J");
    if (clearExc(env) || !mStart || !mQuery) { r.err = "download shim missing (WispActivity)"; return r; }
    // re-attach to a request that survived an app restart
    long long id = -1;
    std::string idFile = path + ".dmid";
    if (FILE* f = std::fopen(idFile.c_str(), "r")) {
        if (std::fscanf(f, "%lld", &id) != 1) id = -1;
        std::fclose(f);
    }
    auto query = [&](long long qid, long long out[4]) {
        jlongArray a = static_cast<jlongArray>(env->CallStaticObjectMethod(j.cls, mQuery, jlong(qid)));
        if (clearExc(env) || !a) return false;
        jlong v[4] = {0, 0, 0, 0};
        env->GetLongArrayRegion(a, 0, 4, v);
        env->DeleteLocalRef(a);
        for (int i = 0; i < 4; ++i) out[i] = v[i];
        return true;
    };
    long long q[4];  // status (DownloadManager.STATUS_*; 0 = unknown id), bytes so far, total, reason
    if (id >= 0 && (!query(id, q) || q[0] == 0 || q[0] == 16)) id = -1;
    if (id < 0) {
        jstring ju = env->NewStringUTF(url.c_str()), jp = env->NewStringUTF(path.c_str()), jt = env->NewStringUTF(title.c_str());
        id = env->CallStaticLongMethod(j.cls, mStart, ju, jp, jt);
        env->DeleteLocalRef(ju); env->DeleteLocalRef(jp); env->DeleteLocalRef(jt);
        if (clearExc(env) || id < 0) { r.err = "DownloadManager refused the request"; return r; }
        if (FILE* f = std::fopen(idFile.c_str(), "w")) { std::fprintf(f, "%lld\n", id); std::fclose(f); }
    }
    for (;;) {
        if (!query(id, q)) { r.err = "download query failed"; return r; }
        if (bytes) *bytes = q[1];
        if (q[0] == 8) break;  // STATUS_SUCCESSFUL
        if (q[0] == 16 || q[0] == 0) {  // STATUS_FAILED or removed
            std::remove(idFile.c_str());
            r.err = "download failed (DownloadManager reason " + std::to_string(q[3]) + ")";
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::remove(idFile.c_str());
    r.sha256 = sha256File(path);
    if (r.sha256.empty()) { r.err = "cannot read the downloaded file"; return r; }
    r.ok = true;
    return r;
}
#endif

}  // namespace wisp
