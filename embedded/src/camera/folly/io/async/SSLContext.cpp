#include "camera/folly/io/async/SSLContext.h"

namespace folly {

namespace {

// GError -> owned std::string, freeing the error.
std::string consume_error(GError* err, const char* what) {
    std::string msg = std::string("tls: ") + what + ": " +
                      (err != nullptr ? err->message : "unknown error");
    if (err != nullptr)
        g_error_free(err);
    return msg;
}

}  // namespace

Expected<SSLContext, std::string> SSLContext::create(
    const std::string& cert_path, const std::string& key_path,
    const std::string& ca_path) {
    SSLContext ctx;
    if (cert_path.empty() && key_path.empty() && ca_path.empty())
        return ctx;  // TLS off

    if (cert_path.empty() || key_path.empty())
        return makeUnexpected(std::string(
            "tls: tls-cert and tls-key must both be set (or both unset)"));

    if (!g_tls_backend_supports_tls(g_tls_backend_get_default()))
        return makeUnexpected(std::string(
            "tls: no GIO TLS backend (is glib-networking installed?)"));

    if (auto r = ctx.loadCertKeyPairFromFiles(cert_path.c_str(),
                                              key_path.c_str());
        !r)
        return makeUnexpected(std::move(r.error()));

    if (!ca_path.empty()) {
        if (auto r = ctx.loadTrustedCertificates(ca_path.c_str()); !r)
            return makeUnexpected(std::move(r.error()));
        ctx.setVerificationOption(VerifyClientCertificate::ALWAYS);
    }
    return ctx;
}

SSLContext::~SSLContext() {
    if (cert_ != nullptr)
        g_object_unref(cert_);
    if (ca_ != nullptr)
        g_object_unref(ca_);
}

Expected<Unit, std::string> SSLContext::loadCertKeyPairFromFiles(
    const char* cert_path, const char* key_path) {
    GError* err = nullptr;
    GTlsCertificate* cert =
        g_tls_certificate_new_from_files(cert_path, key_path, &err);
    if (cert == nullptr)
        return makeUnexpected(consume_error(err, "loading cert/key"));
    if (cert_ != nullptr)
        g_object_unref(cert_);
    cert_ = cert;
    return unit;
}

Expected<Unit, std::string> SSLContext::loadTrustedCertificates(
    const char* ca_path) {
    GError* err = nullptr;
    GTlsDatabase* ca = g_tls_file_database_new(ca_path, &err);
    if (ca == nullptr)
        return makeUnexpected(consume_error(err, "loading ca"));
    if (ca_ != nullptr)
        g_object_unref(ca_);
    ca_ = ca;
    return unit;
}

Expected<GIOStream*, std::string> SSLContext::wrapServerConnection(
    GIOStream* base) const {
    GError* err = nullptr;
    GIOStream* tls = g_tls_server_connection_new(base, cert_, &err);
    if (tls == nullptr)
        return makeUnexpected(consume_error(err, "wrap"));
    if (verify_ == VerifyClientCertificate::ALWAYS && ca_ != nullptr) {
        // Peer chain is validated against |ca_| during the handshake;
        // the default "accept-certificate" handler rejects on any error.
        g_tls_connection_set_database(G_TLS_CONNECTION(tls), ca_);
        g_object_set(tls, "authentication-mode",
                     G_TLS_AUTHENTICATION_REQUIRED, nullptr);
    }
    return tls;
}

}  // namespace folly
