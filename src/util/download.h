#pragma once
// One-file HTTP(S) download with in-process SHA-256.
//   desktop: curl streams the body to stdout; bytes are written and hashed as they arrive.
//   Android: DownloadManager through the Java shim (org.projectwisp.app.WispActivity): it resumes and survives the app
//            being backgrounded or killed (the request id is kept next to the file as <path>.dmid and re-attached);
//            the finished file is then hashed in-process.
#include <atomic>
#include <string>

namespace wisp {

struct DownloadResult {
    bool ok = false;
    std::string sha256;  // hex digest of the downloaded bytes
    std::string err;
};

// Downloads url to path (replacing it). *bytes tracks bytes received so far (may be null).
DownloadResult downloadFile(const std::string& url, const std::string& path, const std::string& title, std::atomic<long long>* bytes);

}  // namespace wisp
