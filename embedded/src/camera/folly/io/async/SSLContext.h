/*
 * API mimic of folly/io/async/SSLContext.h (github.com/facebook/folly),
 * implemented from scratch over GLib TLS (GTlsCertificate, GTlsDatabase,
 * GTlsServerConnection — gnutls via glib-networking) — mimicked, not
 * copied: none of Meta's OpenSSL implementation is used, and the crypto
 * itself stays in the platform TLS library (never hand-rolled).
 *
 * Folly API kept: loadCertKeyPairFromFiles(), loadTrustedCertificates(),
 * setVerificationOption(VerifyClientCertificate). Adapted: errors return
 * folly::Expected instead of throwing. Extensions for the GLib backend:
 * create() (config paths -> ready context), enabled(), and
 * wrapServerConnection() (folly hands out SSL*; our unit of I/O is the
 * GIOStream).
 *
 * Security model ("secure USB"): the USB link is a CDC-NCM network, so
 * the channels that command the device are TCP sockets. With cert+key
 * loaded they speak TLS; with a trusted-CA loaded and verification
 * ALWAYS, the peer must present a certificate signed by that CA (mTLS).
 * The handshake happens implicitly on the first read/write of the
 * wrapped stream (GTlsConnection behavior).
 */
#pragma once

#include <gio/gio.h>

#include <string>
#include <utility>

#include "camera/folly/Expected.h"
#include "camera/folly/Unit.h"

namespace folly {

class SSLContext {
public:
    enum class VerifyClientCertificate {
        ALWAYS,          // require and verify a client certificate
        DO_NOT_REQUEST,  // plain server-authenticated TLS (default)
    };

    // All paths empty -> disabled context (enabled() == false). cert+key
    // set -> server TLS. ca also set -> client certs required (mTLS).
    // cert without key (or vice versa), or unreadable files -> error: a
    // misconfigured security setup must fail startup, never fall back to
    // plaintext.
    static Expected<SSLContext, std::string> create(
        const std::string& cert_path, const std::string& key_path,
        const std::string& ca_path);

    SSLContext() = default;
    ~SSLContext();

    SSLContext(const SSLContext&) = delete;
    SSLContext& operator=(const SSLContext&) = delete;

    SSLContext(SSLContext&& other) noexcept { swap(other); }
    SSLContext& operator=(SSLContext&& other) noexcept {
        SSLContext(std::move(other)).swap(*this);
        return *this;
    }

    /// Loads the server certificate + private key (PEM).
    Expected<Unit, std::string> loadCertKeyPairFromFiles(
        const char* cert_path, const char* key_path);

    /// Loads the CA bundle client certificates are verified against.
    Expected<Unit, std::string> loadTrustedCertificates(const char* ca_path);

    void setVerificationOption(VerifyClientCertificate verify) {
        verify_ = verify;
    }

    bool enabled() const { return cert_ != nullptr; }

    /// Returns a new server-side TLS stream over |base| (transfer full;
    /// takes its own ref on |base|). With verification ALWAYS, the
    /// eventual handshake requires and verifies the client certificate.
    Expected<GIOStream*, std::string> wrapServerConnection(
        GIOStream* base) const;

    void swap(SSLContext& other) noexcept {
        std::swap(cert_, other.cert_);
        std::swap(ca_, other.ca_);
        std::swap(verify_, other.verify_);
    }

private:
    GTlsCertificate* cert_ = nullptr;
    GTlsDatabase* ca_ = nullptr;
    VerifyClientCertificate verify_ = VerifyClientCertificate::DO_NOT_REQUEST;
};

}  // namespace folly
