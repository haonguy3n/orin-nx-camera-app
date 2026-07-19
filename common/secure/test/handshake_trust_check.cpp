// Self-check for ClientHandshake::complete() trust-anchor verification.
// Run from the repo root (one line):
//   g++ -std=c++17 -I common -I embedded/src
//   common/secure/test/handshake_trust_check.cpp common/secure/SecureHandshake.cpp
//   embedded/src/camera/secure/KeySchedule.cpp
//   -lcrypto -o /tmp/handshake_trust_check && /tmp/handshake_trust_check
//
// Covers the trust model that verify_against_trust_anchor() implements: a CA
// anchor accepts any device it signed, a self-signed anchor accepts only
// itself (pinning), and an unrelated anchor accepts neither.
#include <cassert>
#include <cstdlib>
#include <string>
#include <vector>

#include "camera/secure/SecureRecord.h"
#include "secure/SecureHandshake.h"
#include "secure/SecureWire.h"

namespace {

const std::string kDir = "/tmp/handshake_trust_check.d";

void sh(const std::string& command) {
    assert(std::system((command + " >/dev/null 2>&1").c_str()) == 0);
}

// `name` is both the file prefix inside kDir and the certificate CN; it must
// stay slash-free, since openssl -subj uses / as the field separator.
std::string path(const std::string& name) { return kDir + "/" + name; }

void make_ca(const std::string& name) {
    sh("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes"
       " -keyout " + path(name) + ".key -out " + path(name) + ".crt -days 3650"
       " -subj /CN=" + name + " -addext basicConstraints=critical,CA:TRUE");
}

void make_signed_device(const std::string& ca, const std::string& name) {
    sh("openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes"
       " -keyout " + path(name) + ".key -out " + path(name) + ".csr"
       " -subj /CN=" + name);
    sh("openssl x509 -req -in " + path(name) + ".csr -CA " + path(ca) + ".crt"
       " -CAkey " + path(ca) + ".key -CAcreateserial -out " + path(name) + ".crt"
       " -days 3650");
}

void make_self_signed(const std::string& name) {
    sh("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes"
       " -keyout " + path(name) + ".key -out " + path(name) + ".crt -days 3650"
       " -subj /CN=" + name);
}

// One full handshake against `trust_anchor`; true when the client accepted it.
bool handshake_accepted(const std::string& device_cert, const std::string& device_key,
                        const std::string& trust_anchor) {
    auto client = camera::secure::ClientHandshake::create();
    assert(client.hasValue());
    const auto hello = client.value()->client_hello();
    assert(hello.hasValue());
    auto response = camera::secure::DeviceHandshake::respond(
        hello.value(), device_cert, device_key);
    assert(response.hasValue());  // the device side never depends on the anchor
    return client.value()->complete(response.value().first, trust_anchor).hasValue();
}

// Framing: a bulk transfer carries an arbitrary slice of the record stream,
// so reassembly must survive both splitting and coalescing.  Getting this
// wrong killed the session as soon as a channel pushed sustained data.
void check_framing() {
    const camera::secure::SecureRecord::Key key{};
    const camera::secure::SecureRecord::Iv iv{};
    camera::secure::SecureRecord sender(key, iv), receiver(key, iv);

    // Two records: one large enough to be split across transfers, one tiny.
    const std::vector<uint8_t> big(40 * 1024, 0xAB);
    const std::vector<uint8_t> small{'o', 'k'};
    auto first = camera::secure::make_wire_record(
        camera::secure::Channel::Update, 0, big, sender);
    auto second = camera::secure::make_wire_record(
        camera::secure::Channel::Control, 0, small, sender);
    assert(first.hasValue() && second.hasValue());

    std::vector<uint8_t> stream = first.value();
    stream.insert(stream.end(), second.value().begin(), second.value().end());

    // Feed the concatenated stream in 4 KiB chunks: no chunk is a record.
    camera::secure::WireBuffer buffer;
    std::vector<std::vector<uint8_t>> records;
    for (size_t offset = 0; offset < stream.size(); offset += 4096) {
        const size_t size = std::min<size_t>(4096, stream.size() - offset);
        buffer.append(stream.data() + offset, size);
        std::vector<uint8_t> record;
        for (;;) {
            auto framed = buffer.next(&record);
            assert(framed.hasValue());
            if (!framed.value())
                break;
            records.push_back(record);
        }
    }
    assert(records.size() == 2);

    // Both must decrypt, in order, with the payloads intact.
    auto a = camera::secure::open_wire_record(records[0], receiver);
    auto b = camera::secure::open_wire_record(records[1], receiver);
    assert(a.hasValue() && a.value().channel == camera::secure::Channel::Update);
    assert(a.value().payload == big);
    assert(b.hasValue() && b.value().channel == camera::secure::Channel::Control);
    assert(b.value().payload == small);

    // A length no valid peer could send is rejected rather than buffered.
    camera::secure::WireBuffer hostile;
    const uint8_t huge[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    hostile.append(huge, sizeof(huge));
    std::vector<uint8_t> ignored;
    assert(!hostile.next(&ignored).hasValue());
}

// A ClientHello may arrive where a record was expected: the host restarts, or
// gives up on a ServerHello it never saw. The device must recognise it rather
// than read the magic as a record length -- doing so desynchronised the
// endpoint permanently.
void check_rehandshake() {
    // The discrimination is only sound because no valid record length can
    // begin with the magic's first byte. Assert it rather than assume it.
    assert(camera::secure::kMaxWireRecord
           < (static_cast<uint32_t>(camera::secure::kClientHelloMagic[0]) << 24));

    const camera::secure::SecureRecord::Key key{};
    const camera::secure::SecureRecord::Iv iv{};
    camera::secure::SecureRecord sender(key, iv), receiver(key, iv);

    const std::vector<uint8_t> payload{'d', 'a', 't', 'a'};
    auto record = camera::secure::make_wire_record(
        camera::secure::Channel::Video, 1, payload, sender);
    assert(record.hasValue());

    // A live session's record, immediately followed by a fresh handshake.
    std::vector<uint8_t> hello(camera::secure::kClientHelloSize, 0x5A);
    std::copy(std::begin(camera::secure::kClientHelloMagic),
              std::end(camera::secure::kClientHelloMagic), hello.begin());

    camera::secure::WireBuffer stream;
    stream.append(record.value().data(), record.value().size());
    stream.append(hello.data(), hello.size());

    // The record still decodes...
    std::vector<uint8_t> framed_record;
    auto framed = stream.next(&framed_record);
    assert(framed.hasValue() && framed.value());
    auto message = camera::secure::open_wire_record(framed_record, receiver);
    assert(message.hasValue() && message.value().payload == payload);
    // The stream index must survive: it is what lets both cameras share the
    // video channel instead of one evicting the other.
    assert(message.value().channel == camera::secure::Channel::Video);
    assert(message.value().stream == 1);

    // ...and what follows is recognised as a handshake, not a record length.
    assert(stream.starts_with(camera::secure::kClientHelloMagic,
                              sizeof(camera::secure::kClientHelloMagic)));
    assert(stream.size() == camera::secure::kClientHelloSize);
    stream.consume(camera::secure::kClientHelloSize);
    assert(stream.size() == 0);

    // A partially arrived handshake must not be mistaken for a record either.
    camera::secure::WireBuffer partial;
    partial.append(hello.data(), 2);
    std::vector<uint8_t> unused;
    auto pending = partial.next(&unused);
    assert(pending.hasValue() && !pending.value());  // waiting, not an error
}

// The gadget appends zero padding to some transfers. Skipping it must not
// touch a real record: a 74-byte one is 00 00 00 4a on the wire, so three of
// its four header bytes are already zero.
void check_zero_padding() {
    const camera::secure::SecureRecord::Key key{};
    const camera::secure::SecureRecord::Iv iv{};
    camera::secure::SecureRecord sender(key, iv), receiver(key, iv);

    // 53 bytes of payload -> a 74-byte wire record, matching what the control
    // channel actually sends.
    const std::vector<uint8_t> payload(53, 'x');
    auto record = camera::secure::make_wire_record(
        camera::secure::Channel::Control, 0, payload, sender);
    assert(record.hasValue());
    assert(record.value().size() == 75);  // +1 for the stream byte
    assert(record.value()[0] == 0 && record.value()[1] == 0 && record.value()[2] == 0);

    // Padding ahead of it is dropped; the record itself survives intact.
    camera::secure::WireBuffer stream;
    const std::vector<uint8_t> padding(411, 0);
    stream.append(padding.data(), padding.size());
    stream.append(record.value().data(), record.value().size());
    assert(stream.skip_zero_padding() == padding.size());

    std::vector<uint8_t> framed_record;
    auto framed = stream.next(&framed_record);
    assert(framed.hasValue() && framed.value());
    auto message = camera::secure::open_wire_record(framed_record, receiver);
    assert(message.hasValue() && message.value().payload == payload);
    assert(stream.size() == 0);

    // With no padding present it must be a no-op, not a consumer of headers.
    camera::secure::SecureRecord second_sender(key, iv), second_receiver(key, iv);
    auto clean = camera::secure::make_wire_record(
        camera::secure::Channel::Control, 0, payload, second_sender);
    assert(clean.hasValue());
    camera::secure::WireBuffer untouched;
    untouched.append(clean.value().data(), clean.value().size());
    assert(untouched.skip_zero_padding() == 0);
    assert(untouched.size() == clean.value().size());
    std::vector<uint8_t> intact;
    auto still = untouched.next(&intact);
    assert(still.hasValue() && still.value());
    assert(camera::secure::open_wire_record(intact, second_receiver).hasValue());
}

}  // namespace

int main() {
    check_framing();
    check_rehandshake();
    check_zero_padding();

    sh("rm -rf " + kDir + " && mkdir -p " + kDir);

    make_ca("real-ca");
    make_ca("other-ca");
    make_signed_device("real-ca", "device");
    make_self_signed("pinned");
    make_self_signed("impostor");

    const std::string device_crt = path("device") + ".crt";
    const std::string device_key = path("device") + ".key";

    // CA anchor: accepts a device the CA signed.
    assert(handshake_accepted(device_crt, device_key, path("real-ca") + ".crt"));

    // ...and rejects one signed by a different CA. Without this the CA check
    // would be decorative.
    assert(!handshake_accepted(device_crt, device_key, path("other-ca") + ".crt"));

    // Self-signed anchor: pinning still works through the same code path.
    assert(handshake_accepted(path("pinned") + ".crt", path("pinned") + ".key",
                              path("pinned") + ".crt"));

    // A different self-signed device must not satisfy that pin.
    assert(!handshake_accepted(path("impostor") + ".crt", path("impostor") + ".key",
                               path("pinned") + ".crt"));

    // A CA-signed device must not satisfy a pin on an unrelated certificate.
    assert(!handshake_accepted(device_crt, device_key, path("pinned") + ".crt"));

    // A missing anchor file is a failure, never an accept-by-default.
    assert(!handshake_accepted(device_crt, device_key, path("absent") + ".crt"));

    sh("rm -rf " + kDir);
    return 0;
}
