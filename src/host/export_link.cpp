#include "export_link.h"

#include <logos_protocol.h>

#include <nlohmann/json.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace logos::native_host {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr int kCallTimeoutMs = 10000;
// No word from peering_module for this long: every session closes.
constexpr auto kUnreachableLimit = std::chrono::seconds(60);

char* heapCopy(const std::string& text)
{
    auto* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out) std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

std::string bioText(BIO* bio)
{
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    return size > 0 ? std::string(data, static_cast<std::size_t>(size)) : std::string();
}

// A P-256 key (PKCS#8 PEM) and a CSR proving possession of it.
bool makeKeyAndCsr(std::string& keyPem, std::string& csrPem)
{
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    X509_REQ* request = X509_REQ_new();
    BIO* keyBio = BIO_new(BIO_s_mem());
    BIO* csrBio = BIO_new(BIO_s_mem());
    const bool ok = key && request && keyBio && csrBio && X509_REQ_set_version(request, 0) == 1
        && X509_REQ_set_pubkey(request, key) == 1 && X509_REQ_sign(request, key, EVP_sha256()) > 0
        && PEM_write_bio_PrivateKey(keyBio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1
        && PEM_write_bio_X509_REQ(csrBio, request) == 1;
    if (ok) {
        keyPem = bioText(keyBio);
        csrPem = bioText(csrBio);
    }
    BIO_free(csrBio);
    BIO_free(keyBio);
    X509_REQ_free(request);
    EVP_PKEY_free(key);
    return ok && !keyPem.empty() && !csrPem.empty();
}

} // namespace

struct ExportLink::State {
    std::string module;
    std::string peering;
    std::chrono::milliseconds reconcile;
    lp_client* client = nullptr;
    lp_provider* provider = nullptr;
    std::vector<lp_subscription*> subscriptions;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;
    Clock::time_point lastHeard = Clock::now();

    // The result of `method`, or {"error": ...}.
    json call(const std::string& method, const json& args)
    {
        char* out = nullptr;
        char* err = nullptr;
        const std::string text = args.dump();
        const int status = lp_invoke(client, method.c_str(), text.c_str(), kCallTimeoutMs, &out, &err);
        json result = json{{"error", "UNREACHABLE"}};
        if (status == LP_OK && out) {
            auto parsed = json::parse(out, nullptr, false);
            if (parsed.is_object()) result = std::move(parsed);
        }
        lp_string_free(out);
        lp_string_free(err);
        if (!result.contains("error")) {
            std::lock_guard<std::mutex> lock(mutex);
            lastHeard = Clock::now();
        }
        return result;
    }

    void setAnchors(const json& reply)
    {
        const auto it = reply.find("anchors_pem");
        if (it != reply.end() && it->is_string())
            lp_provider_set_trust_anchors(provider, it->get<std::string>().c_str());
    }

    // Sessions below a peer's revocation generation go.
    void applyGenerations(const json& generations)
    {
        if (!generations.is_object()) return;
        for (const auto& item : generations.items())
            if (item.value().is_number_integer()) {
                const std::string filter =
                    json{{"peer", item.key()}, {"generation_below", item.value()}}.dump();
                lp_provider_close_sessions(provider, filter.c_str());
            }
    }

    static char* authenticate(const char* request, void* userData)
    {
        auto* state = static_cast<State*>(userData);
        const json parsed = json::parse(request ? request : "", nullptr, false);
        if (!parsed.is_object()) return heapCopy(R"({"error":"NOT_AUTHORISED"})");
        return heapCopy(state->call("redeemTicket", json::array({parsed})).dump());
    }

    static void onEvent(const char* event, const char* data, void* userData)
    {
        auto* state = static_cast<State*>(userData);
        const json args = json::parse(data ? data : "[]", nullptr, false);
        const std::string name = event ? event : "";
        if (name == "anchorsChanged") {
            state->setAnchors(state->call("sessionAnchors", json::array()));
        } else if (name == "routesRevoked" && args.is_array() && args.size() >= 2
                   && args[0].is_string() && args[1].is_number_integer()) {
            state->applyGenerations(json{{args[0].get<std::string>(), args[1]}});
        } else if (name == "routeRenewed" && args.is_array() && args.size() >= 2
                   && args[0].is_string() && args[1].is_number_integer()) {
            const std::string filter = json{{"route", args[0]}}.dump();
            lp_provider_extend_sessions(state->provider, filter.c_str(), args[1].get<long long>());
        }
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            wake.wait_for(lock, reconcile);
            if (stopping) break;
            lock.unlock();
            const json reply = call("sessionState", json::array());
            if (!reply.contains("error")) {
                setAnchors(reply);
                applyGenerations(reply.value("generations", json::object()));
            }
            lock.lock();
            if (Clock::now() - lastHeard >= kUnreachableLimit) {
                lock.unlock();
                lp_provider_close_sessions(provider, "{}");
                lock.lock();
            }
        }
    }
};

ExportLink::ExportLink(std::string module, std::string peering, std::chrono::milliseconds reconcile)
    : m_state(std::make_shared<State>())
{
    m_state->module = std::move(module);
    m_state->peering = std::move(peering);
    m_state->reconcile = reconcile;
}

ExportLink::~ExportLink()
{
    stop();
    if (m_state->client) lp_client_destroy(m_state->client);
}

bool ExportLink::configure(lp_provider* provider, std::string& error)
{
    State& s = *m_state;
    s.provider = provider;
    s.client = lp_client_create(s.peering.c_str(), s.module.c_str(), nullptr, nullptr);
    if (!s.client) {
        error = "export: no client for " + s.peering;
        return false;
    }
    std::string keyPem;
    std::string csrPem;
    if (!makeKeyAndCsr(keyPem, csrPem)) {
        error = "export: could not make a key";
        return false;
    }
    const json issued = s.call("issueCertificate", json::array({"provider", csrPem}));
    const auto chain = issued.find("chain_pem");
    if (chain == issued.end() || !chain->is_string()) {
        error = "export: " + s.peering + " issued no certificate ("
            + issued.value("error", std::string("no answer")) + ")";
        return false;
    }
    if (lp_provider_set_tls_credential(provider, chain->get<std::string>().c_str(), keyPem.c_str())
            != LP_OK
        || lp_provider_set_session_authenticator(provider, &State::authenticate, &s) != LP_OK) {
        error = "export: the provider refused its session credential";
        return false;
    }
    // Where the runtime's exports listen (its port range), set before the listener starts.
    if (const auto options = issued.find("session_options");
        options != issued.end() && options->is_object()
        && lp_provider_set_session_options(provider, options->dump().c_str()) != LP_OK) {
        error = "export: the provider refused its session options";
        return false;
    }
    s.setAnchors(issued);
    return true;
}

bool ExportLink::published(std::string& error)
{
    State& s = *m_state;
    char* text = lp_provider_endpoints_json(s.provider);
    const json endpoints = json::parse(text ? text : "[]", nullptr, false);
    lp_string_free(text);
    const json noted = s.call("noteEndpoints", json::array({endpoints}));
    if (noted.contains("error")) {
        error = "export: " + s.peering + " refused the endpoints (" + noted.value("error", "") + ")";
        return false;
    }
    for (const char* event : {"anchorsChanged", "routesRevoked", "routeRenewed"})
        if (lp_subscription* sub = lp_subscribe(s.client, event, &State::onEvent, &s))
            s.subscriptions.push_back(sub);
    auto state = m_state;
    s.worker = std::thread([state] { state->run(); });
    return true;
}

void ExportLink::stop()
{
    State& s = *m_state;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stopping = true;
    }
    s.wake.notify_all();
    if (s.worker.joinable()) s.worker.join();
    for (lp_subscription* sub : s.subscriptions) lp_unsubscribe(sub);
    s.subscriptions.clear();
}

} // namespace logos::native_host
