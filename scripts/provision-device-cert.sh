#!/usr/bin/env bash
# Issue a camera device certificate signed by a local CA.
#
# Step one of moving off the self-signed cert that camera-streamer-gencert.
# service generates on first boot (which nothing can verify, so the host has
# to pin it).  Here the host verifies against ca.crt instead, and a camera
# can be replaced without re-pairing every host.
#
# The CA private key stays in --ca-dir and is never copied into the image or
# this repository.  Only ca.crt (public) is shipped.
#
#   ./scripts/provision-device-cert.sh --device-id cam-0001 --out /tmp/cert
#   ./scripts/provision-device-cert.sh --self-test
#
# Then point the Yocto build at the output (see the camera-device-cert
# recipe): CAMERA_DEVICE_CERT_DIR = "/tmp/cert" in local.conf.
set -euo pipefail

CA_DIR="${CAMERA_CA_DIR:-$HOME/.config/camera-ca}"
OUT_DIR=""
DEVICE_ID=""
DEVICE_DAYS=3650
CA_DAYS=7300          # the CA must outlive every cert it signs
SELF_TEST=0

die() { echo "error: $*" >&2; exit 1; }

usage() {
    sed -n '2,17p' "$0" | sed 's/^# \?//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --ca-dir)    CA_DIR="${2:?--ca-dir needs a path}"; shift 2 ;;
        --out)       OUT_DIR="${2:?--out needs a path}"; shift 2 ;;
        --device-id) DEVICE_ID="${2:?--device-id needs a value}"; shift 2 ;;
        --days)      DEVICE_DAYS="${2:?--days needs a value}"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        -h|--help)   usage 0 ;;
        *)           die "unknown argument: $1 (try --help)" ;;
    esac
done

# Create the CA on first use.  P-256 to match what the device already uses
# and what SecureHandshake expects.
ensure_ca() {
    if [ -s "$CA_DIR/ca.key" ] && [ -s "$CA_DIR/ca.crt" ]; then
        return
    fi
    # Half a CA means an interrupted run; regenerating would silently orphan
    # every certificate already issued from it.
    if [ -e "$CA_DIR/ca.key" ] || [ -e "$CA_DIR/ca.crt" ]; then
        die "$CA_DIR holds a partial CA; refusing to overwrite it"
    fi
    echo "creating CA in $CA_DIR"
    mkdir -p "$CA_DIR"
    chmod 700 "$CA_DIR"
    ( umask 077
      openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
          -keyout "$CA_DIR/ca.key" -out "$CA_DIR/ca.crt" \
          -days "$CA_DAYS" -subj "/CN=camera-app device CA" \
          -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
          -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null )
    chmod 600 "$CA_DIR/ca.key"
    chmod 644 "$CA_DIR/ca.crt"
}

issue_device_cert() {
    local id="$1" out="$2"
    mkdir -p "$out"
    ( umask 077
      openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
          -keyout "$out/server.key" -out "$out/server.csr" \
          -subj "/CN=$id" 2>/dev/null )

    # SANs cover both transports: the secure USB proxy reaches the servers on
    # 127.0.0.1, the plain NCM path uses the gadget's static 192.168.55.1.
    # Without these, GIO's TLS backend rejects the cert on hostname check.
    openssl x509 -req -in "$out/server.csr" \
        -CA "$CA_DIR/ca.crt" -CAkey "$CA_DIR/ca.key" -CAcreateserial \
        -out "$out/server.crt" -days "$DEVICE_DAYS" \
        -extfile <(printf '%s\n' \
            "basicConstraints=critical,CA:FALSE" \
            "keyUsage=critical,digitalSignature,keyEncipherment" \
            "extendedKeyUsage=serverAuth" \
            "subjectAltName=DNS:$id,DNS:camera-streamer,IP:192.168.55.1,IP:127.0.0.1") \
        2>/dev/null

    rm -f "$out/server.csr"
    cp "$CA_DIR/ca.crt" "$out/ca.crt"
    chmod 600 "$out/server.key"
    chmod 644 "$out/server.crt" "$out/ca.crt"

    # Never hand back a chain that does not verify.
    openssl verify -CAfile "$out/ca.crt" "$out/server.crt" >/dev/null \
        || die "issued certificate does not verify against the CA"
}

# Runnable check: a full issue cycle in a throwaway dir, asserting that the
# chain verifies and that an unrelated CA does NOT verify it.
self_test() {
    # Not local: the EXIT trap runs after this function's scope is gone.
    TEST_DIR="$(mktemp -d)"
    trap 'rm -rf "$TEST_DIR"' EXIT
    local tmp="$TEST_DIR"
    CA_DIR="$tmp/ca" ensure_ca
    CA_DIR="$tmp/ca" issue_device_cert "cam-selftest" "$tmp/out"

    openssl verify -CAfile "$tmp/out/ca.crt" "$tmp/out/server.crt" >/dev/null \
        || die "self-test: chain should verify"

    # A cert from a different CA must be rejected, or verification is a no-op.
    CA_DIR="$tmp/other" ensure_ca
    if openssl verify -CAfile "$tmp/other/ca.crt" "$tmp/out/server.crt" \
           >/dev/null 2>&1; then
        die "self-test: foreign CA should NOT verify this cert"
    fi

    # The key must not be world-readable: it ends up in a rootfs.
    [ "$(stat -c %a "$tmp/out/server.key")" = "600" ] \
        || die "self-test: server.key permissions are too open"

    openssl x509 -in "$tmp/out/server.crt" -noout -text \
        | grep -q "CA:FALSE" || die "self-test: device cert must not be a CA"

    echo "self-test: ok"
}

if [ "$SELF_TEST" = 1 ]; then
    self_test
    exit 0
fi

[ -n "$DEVICE_ID" ] || die "--device-id is required (e.g. the board serial)"
[ -n "$OUT_DIR" ]   || die "--out is required"

ensure_ca
issue_device_cert "$DEVICE_ID" "$OUT_DIR"

cat <<EOF
issued device certificate for "$DEVICE_ID"

  $OUT_DIR/server.crt   device certificate  (ships in the image)
  $OUT_DIR/server.key   device private key  (ships in the image, mode 0600)
  $OUT_DIR/ca.crt       CA certificate      (ships; also give this to hosts)

  $CA_DIR/ca.key        CA PRIVATE KEY -- never copy this into the image,
                        the repository, or a host. Back it up offline: losing
                        it means re-provisioning every camera.

Build the image with this cert:
  CAMERA_DEVICE_CERT_DIR = "$OUT_DIR"      # in local.conf

NOTE: every board flashed with that image shares this identity. Per-device
certs need per-device provisioning at flash time -- not yet implemented.
EOF
