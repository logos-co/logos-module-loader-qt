// An exported module's tls_tcp listener, certified and authenticated by a
// stand-in peering_module served from this test: the host calls it as the
// module, and a revocation it announces closes the sessions it covers.
#include "native_module_host.h"

#include <logos_protocol.h>

#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using logos::native_host::Module;
using logos::native_host::Options;
using logos::native_host::Teardown;

struct KeyFree { void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); } };
struct CertFree { void operator()(X509* c) const { X509_free(c); } };
using Key = std::unique_ptr<EVP_PKEY, KeyFree>;
using Cert = std::unique_ptr<X509, CertFree>;

Key newKey() { return Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256")); }

void addExt(X509* cert, X509* issuer, int nid, const char* value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (!ext) throw std::runtime_error("extension");
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
}

// A root when `issuer` is null, else a leaf for `subject` with `eku`.
Cert makeCert(EVP_PKEY* subject, EVP_PKEY* signer, X509* issuer, const char* eku)
{
    Cert cert(X509_new());
    X509_set_version(cert.get(), 2);
    static long serial = 5000;
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), ++serial);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(cert.get()), "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("export-test"), -1, -1, 0);
    X509* issuerCert = issuer ? issuer : cert.get();
    X509_set_issuer_name(cert.get(), X509_get_subject_name(issuerCert));
    X509_set_pubkey(cert.get(), subject);
    if (!issuer) {
        addExt(cert.get(), issuerCert, NID_basic_constraints, "critical,CA:TRUE,pathlen:0");
        addExt(cert.get(), issuerCert, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        addExt(cert.get(), issuerCert, NID_basic_constraints, "critical,CA:FALSE");
        addExt(cert.get(), issuerCert, NID_key_usage, "critical,digitalSignature");
        addExt(cert.get(), issuerCert, NID_ext_key_usage, eku);
    }
    addExt(cert.get(), issuerCert, NID_subject_key_identifier, "hash");
    addExt(cert.get(), issuerCert, NID_authority_key_identifier, "keyid:always");
    X509_sign(cert.get(), signer, EVP_sha256());
    return cert;
}

std::string pem(X509* cert)
{
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<std::size_t>(size));
    BIO_free(bio);
    return out;
}

std::string pem(EVP_PKEY* key)
{
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<std::size_t>(size));
    BIO_free(bio);
    return out;
}

std::string pin(X509* cert)
{
    unsigned char* der = nullptr;
    const int size = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_Digest(der, static_cast<std::size_t>(size), hash, &length, EVP_sha256(), nullptr);
    OPENSSL_free(der);
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out = "sha256:";
    unsigned acc = 0;
    int bits = 0;
    for (unsigned i = 0; i < length; ++i) {
        acc = ((acc << 8) | hash[i]) & 0xFFFFFFu;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(acc >> bits) & 63u]);
        }
    }
    if (bits > 0) out.push_back(alphabet[(acc << (6 - bits)) & 63u]);
    return out;
}

char* heap(const std::string& text)
{
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

fs::path executableDir()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return fs::path(std::wstring(buffer, length)).parent_path();
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    return _NSGetExecutablePath(buffer, &size) == 0 ? fs::path(buffer).parent_path() : fs::path();
#else
    return fs::read_symlink("/proc/self/exe").parent_path();
#endif
}

std::string fixturePath()
{
    const fs::path dir = executableDir();
    for (const fs::path& candidate : {dir / "plain_host_fixture.dll",
                                      dir / ".." / "lib" / "libplain_host_fixture.so",
                                      dir / ".." / "lib" / "libplain_host_fixture.dylib"}) {
        if (fs::exists(candidate)) return fs::absolute(candidate).lexically_normal().u8string();
    }
    return {};
}

void useInstance(const std::string& prefix)
{
#ifdef _WIN32
    ASSERT_EQ(::_putenv_s("LOGOS_INSTANCE_ID", (prefix + std::to_string(::_getpid())).c_str()), 0);
#else
    ASSERT_EQ(::setenv("LOGOS_INSTANCE_ID", (prefix + std::to_string(::getpid())).c_str(), 1), 0);
#endif
}

// The stand-in: certifies what the host asks for and admits ticket "good".
struct Peering {
    Key serverRootKey = newKey();
    Cert serverRoot = makeCert(serverRootKey.get(), serverRootKey.get(), nullptr, nullptr);
    Key clientRootKey = newKey();
    Cert clientRoot = makeCert(clientRootKey.get(), clientRootKey.get(), nullptr, nullptr);
    Key clientKey = newKey();
    Cert clientLeaf = makeCert(clientKey.get(), clientRootKey.get(), clientRoot.get(), "clientAuth");

    std::mutex mutex;
    std::vector<std::string> callers;
    std::vector<json> redeemed;
    std::string providerPin;
    int port = 0;
    int portRange = 0; // when set, the one port an export may listen on
    lp_provider* provider = nullptr;

    Peering()
    {
        provider = lp_provider_create("peering_module", nullptr);
        EXPECT_EQ(lp_provider_save_token(provider, "plain_host_fixture", "pm-token"), LP_OK);
        EXPECT_EQ(lp_provider_register(provider, &Peering::dispatch, &Peering::methods, nullptr, this),
                  LP_OK);
        // The host presents this cached token rather than asking capability_module.
        EXPECT_EQ(lp_token_save("peering_module", "pm-token"), LP_OK);
    }

    ~Peering() { lp_provider_destroy(provider); }

    std::string clientChain() const { return pem(clientLeaf.get()) + pem(clientRoot.get()); }

    json issue(const json& args)
    {
        const std::string csr = args.at(1).get<std::string>();
        BIO* bio = BIO_new_mem_buf(csr.data(), static_cast<int>(csr.size()));
        X509_REQ* request = PEM_read_bio_X509_REQ(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        EVP_PKEY* key = request ? X509_REQ_get_pubkey(request) : nullptr;
        if (!key || X509_REQ_verify(request, key) != 1 || args.at(0) != "provider") {
            EVP_PKEY_free(key);
            X509_REQ_free(request);
            return {{"error", "INVALID_ARGUMENT"}};
        }
        const Cert leaf = makeCert(key, serverRootKey.get(), serverRoot.get(), "serverAuth");
        EVP_PKEY_free(key);
        X509_REQ_free(request);
        std::lock_guard<std::mutex> lock(mutex);
        providerPin = pin(leaf.get());
        json reply = {{"chain_pem", pem(leaf.get()) + pem(serverRoot.get())},
                      {"anchors_pem", pem(clientRoot.get())}};
        if (portRange) reply["session_options"] = {{"port_min", portRange}, {"port_max", portRange}};
        return reply;
    }

    static char* dispatch(const char* method, const char* argsJson, void* userData)
    {
        auto& self = *static_cast<Peering*>(userData);
        const json args = json::parse(argsJson ? argsJson : "[]");
        {
            std::lock_guard<std::mutex> lock(self.mutex);
            self.callers.push_back(lp_current_caller_json());
        }
        const std::string name = method;
        if (name == "issueCertificate") return heap(self.issue(args).dump());
        if (name == "noteEndpoints") {
            std::lock_guard<std::mutex> lock(self.mutex);
            for (const auto& e : args.at(0))
                if (e.value("protocol", "") == "tls_tcp") self.port = e.value("port", 0);
            return heap(R"({"ok":true})");
        }
        if (name == "redeemTicket") {
            const json& request = args.at(0);
            std::lock_guard<std::mutex> lock(self.mutex);
            self.redeemed.push_back(request);
            if (request["hello"].value("ticket", "") != "good" || !request.value("anchored", false))
                return heap(R"({"error":"NOT_AUTHORISED"})");
            return heap(json{{"caller", {{"kind", "remote"}, {"peer", "peer-1"}, {"name", "wallet"}}},
                             {"lifetime_ms", 60000},
                             {"session", {{"peer", "peer-1"}, {"route", "r1"}, {"generation", 1}}}}
                            .dump());
        }
        if (name == "sessionAnchors") return heap(json{{"anchors_pem", pem(self.clientRoot.get())}}.dump());
        if (name == "sessionState")
            return heap(json{{"anchors_pem", pem(self.clientRoot.get())},
                             {"generations", json::object()}}.dump());
        return heap(R"({"error":"NO_SUCH_METHOD"})");
    }

    static char* methods(void*) { return heap("[]"); }

    int redeemCount()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(redeemed.size());
    }
};

struct Dialer {
    Peering* peering = nullptr;
    std::string ticket = "good";

    static char* dial(const char*, void* userData)
    {
        auto& d = *static_cast<Dialer*>(userData);
        std::lock_guard<std::mutex> lock(d.peering->mutex);
        return heap(json{{"addresses", {"127.0.0.1"}}, {"port", d.peering->port},
                         {"server_pin", d.peering->providerPin},
                         {"anchors", pem(d.peering->serverRoot.get())}}.dump());
    }

    static char* hello(const char*, void* userData)
    {
        auto& d = *static_cast<Dialer*>(userData);
        return heap(json{{"ticket", d.ticket}, {"module", "plain_host_fixture"}}.dump());
    }
};

int invoke(lp_client* client, const char* method, json* result, int timeoutMs = 5000)
{
    char* out = nullptr;
    char* err = nullptr;
    const int status = lp_invoke(client, method, "[]", timeoutMs, &out, &err);
    if (result && out) *result = json::parse(out);
    lp_string_free(out);
    lp_string_free(err);
    return status;
}

Options exportOptions()
{
    Options options;
    options.name = "plain_host_fixture";
    options.path = fixturePath();
    options.transportSet = R"([{"protocol":"inproc"},{"protocol":"tls_tcp","host":"127.0.0.1","port":0}])";
    options.credential = "fixture-credential";
    options.peering = "peering_module";
    return options;
}

TEST(ExportLink, AnExportedModuleIsCertifiedAndAuthenticatedByPeering)
{
    useInstance("export_link_serve_");
    Peering peering;
    Module module;
    std::string error;
    ASSERT_FALSE(fixturePath().empty());
    ASSERT_TRUE(module.start(exportOptions(), error)) << error;
    {
        std::lock_guard<std::mutex> lock(peering.mutex);
        ASSERT_GT(peering.port, 0);
        ASSERT_FALSE(peering.callers.empty());
        // The host asked as the module it serves.
        EXPECT_EQ(json::parse(peering.callers.front()),
                  json({{"kind", "module"}, {"name", "plain_host_fixture"}}));
    }

    Dialer dialer;
    dialer.peering = &peering;
    lp_client* client = lp_client_create("plain_host_fixture", "wallet", R"({"protocol":"tls_tcp"})", nullptr);
    ASSERT_NE(client, nullptr);
    ASSERT_EQ(lp_client_set_tls_credential(client, peering.clientChain().c_str(),
                                           pem(peering.clientKey.get()).c_str()), LP_OK);
    ASSERT_EQ(lp_client_set_session_hook(client, &Dialer::dial, &Dialer::hello, &dialer), LP_OK);
    json who;
    ASSERT_EQ(invoke(client, "caller", &who), LP_OK);
    EXPECT_EQ(who, json({{"kind", "remote"}, {"peer", "peer-1"}, {"name", "wallet"}}));
    EXPECT_EQ(peering.redeemCount(), 1);

    // Announced revocations close the sessions below the new generation.
    bool closed = false;
    for (int i = 0; i < 100 && !closed; ++i) {
        lp_provider_emit_event(peering.provider, "routesRevoked", R"(["peer-1",2])");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        closed = lp_provider_extend_sessions(module.provider(), R"({"route":"r1"})", 60000) == 0;
    }
    EXPECT_TRUE(closed);

    lp_client_destroy(client);
    EXPECT_TRUE(module.stop(std::chrono::steady_clock::now() + std::chrono::seconds(3), Teardown::InProcess));
}

TEST(ExportLink, ABadTicketOpensNoSession)
{
    useInstance("export_link_refuse_");
    Peering peering;
    Module module;
    std::string error;
    ASSERT_TRUE(module.start(exportOptions(), error)) << error;
    Dialer dialer;
    dialer.peering = &peering;
    dialer.ticket = "stale";
    lp_client* client = lp_client_create("plain_host_fixture", "wallet", R"({"protocol":"tls_tcp"})", nullptr);
    lp_client_set_tls_credential(client, peering.clientChain().c_str(), pem(peering.clientKey.get()).c_str());
    lp_client_set_session_hook(client, &Dialer::dial, &Dialer::hello, &dialer);
    json who;
    EXPECT_NE(invoke(client, "caller", &who, 3000), LP_OK);
    EXPECT_GE(peering.redeemCount(), 1);
    lp_client_destroy(client);
    module.stop(std::chrono::steady_clock::now() + std::chrono::seconds(3), Teardown::InProcess);
}

TEST(ExportLink, NoCertificateNoExport)
{
    useInstance("export_link_nocert_");
    Peering peering;
    Options options = exportOptions();
    options.peering = "no_such_peering";
    Module module;
    std::string error;
    EXPECT_FALSE(module.start(options, error));
    EXPECT_NE(error.find("export:"), std::string::npos) << error;
}

} // namespace

namespace {

int freePort()
{
    lp_provider* probe = lp_provider_create("export_link_port_probe",
                                            R"([{"protocol":"tcp","host":"127.0.0.1","port":0}])");
    int port = 0;
    if (probe && lp_provider_register(probe, &Peering::dispatch, &Peering::methods, nullptr, nullptr) == LP_OK) {
        char* text = lp_provider_endpoints_json(probe);
        const json endpoints = json::parse(text ? text : "[]", nullptr, false);
        lp_string_free(text);
        if (endpoints.is_array() && !endpoints.empty()) port = endpoints[0].value("port", 0);
    }
    if (probe) lp_provider_destroy(probe);
    return port;
}

} // namespace

TEST(ExportLink, ListensWhereItsRuntimeExportsListen)
{
    useInstance("export_link_ports_");
    Peering peering;
    peering.portRange = freePort();
    ASSERT_GT(peering.portRange, 0);
    Module module;
    std::string error;
    ASSERT_TRUE(module.start(exportOptions(), error)) << error;
    {
        std::lock_guard<std::mutex> lock(peering.mutex);
        EXPECT_EQ(peering.port, peering.portRange);
    }
    module.stop(std::chrono::steady_clock::now() + std::chrono::seconds(3), Teardown::InProcess);
}
