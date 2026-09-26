#ifndef LOGOS_EXPORT_LINK_H
#define LOGOS_EXPORT_LINK_H

// An exported module's link to peering_module, which it calls as itself: the
// certificate and anchors of its tls_tcp listeners, the authentication of every
// session (redeemTicket), and the revocations and renewals that follow.

#include <chrono>
#include <memory>
#include <string>

struct lp_client;
struct lp_provider;

namespace logos::native_host {

class ExportLink {
public:
    ExportLink(std::string module, std::string peering,
               std::chrono::milliseconds reconcile = std::chrono::seconds(30));
    // The runtime's own endpoint for `module` (core_service's operator
    // listener): calls go on `peering`, which stays the caller's and must
    // outlive this object.
    ExportLink(std::string module, lp_client* peering,
               std::chrono::milliseconds reconcile = std::chrono::seconds(30));
    ~ExportLink();
    ExportLink(const ExportLink&) = delete;
    ExportLink& operator=(const ExportLink&) = delete;

    // Before the provider's tls_tcp listener starts: a fresh key certified as
    // `provider`, the enrolled roots as anchors, and the session authenticator.
    bool configure(lp_provider* provider, std::string& error);
    // After it is published: report the listeners, follow revocations.
    bool published(std::string& error);
    // Before the provider is destroyed: no more events or reconciles. The
    // client that authenticates sessions lives until this object does, so
    // destroy it after the provider.
    void stop();

    struct State;

private:
    std::shared_ptr<State> m_state;
};

} // namespace logos::native_host

#endif
