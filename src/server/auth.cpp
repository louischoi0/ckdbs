#include "kds/server/auth.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

// Compiled only under KDS_WITH_TLS (CMakeLists.txt): the gate needs the
// SCRAM crypto in scram.cpp, which rides the same OpenSSL dependency.

namespace kds::server {

// ---- FileCredentialStore ----------------------------------------------

StatusOr<FileCredentialStore> FileCredentialStore::Load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return Status::NotFound("cannot open users file '" + path +
                                "': " + std::strerror(errno));
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return Parse(buf.str(), path);
}

StatusOr<FileCredentialStore> FileCredentialStore::Parse(std::string_view text,
                                                         const std::string& origin) {
    FileCredentialStore store;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++line_no;

        // Trim, honour '#' comments whole-line or trailing.
        std::size_t hash = line.find('#');
        if (hash != std::string_view::npos) line = line.substr(0, hash);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() &&
               (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) continue;

        std::size_t space = line.find(' ');
        if (space == std::string_view::npos) {
            return Status::InvalidArgument(origin + ":" + std::to_string(line_no) +
                                            ": expected '<username> <verifier>'");
        }
        std::string username(line.substr(0, space));
        std::string_view verifier_text = line.substr(space + 1);

        if (store.users_.find(username) != store.users_.end()) {
            return Status::InvalidArgument(origin + ":" + std::to_string(line_no) + ": user '" +
                                            username + "' is listed twice");
        }
        auto verifier = scram::Verifier::Parse(verifier_text);
        if (!verifier.ok()) {
            return Status::InvalidArgument(origin + ":" + std::to_string(line_no) + ": " +
                                            verifier.status().message());
        }
        store.users_.emplace(std::move(username), std::move(verifier.value()));
    }
    return store;
}

StatusOr<scram::Verifier> FileCredentialStore::Lookup(std::string_view username) const {
    auto it = users_.find(std::string(username));
    if (it == users_.end()) {
        return Status::NotFound("no such user");  // mocked by scram::Server, never shown
    }
    return it->second;
}

// ---- ScramAuthGate ----------------------------------------------------

namespace {

constexpr std::string_view kAuthFirstPrefix = "AUTH SCRAM-SHA-256 ";
constexpr std::string_view kAuthContinuePrefix = "AUTH ";

// One refusal string for every pre-auth failure a client can cause. A
// single spelling on purpose: the difference between "unknown command",
// "unknown mechanism" and "bad message" is reconnaissance, and an
// unauthenticated peer has not earned diagnostics. The server log is
// where the operator's detail belongs (future observability work).
AuthGate::Result Refuse() {
    return {"ERR authentication required (AUTH SCRAM-SHA-256 <client-first-message>)",
            /*authenticated=*/false, /*close=*/true};
}

}  // namespace

ScramAuthGate::ScramAuthGate(const CredentialStore* store)
    : server_([store](std::string_view user) { return store->Lookup(user); }) {}

AuthGate::Result ScramAuthGate::OnLine(std::string_view line) {
    if (!got_first_) {
        if (line.substr(0, kAuthFirstPrefix.size()) != kAuthFirstPrefix) return Refuse();
        StatusOr<std::string> server_first =
            server_.OnClientFirst(line.substr(kAuthFirstPrefix.size()));
        if (!server_first.ok()) return Refuse();
        got_first_ = true;
        return {"AUTH+ " + server_first.value(), false, false};
    }

    if (line.substr(0, kAuthContinuePrefix.size()) != kAuthContinuePrefix) return Refuse();
    StatusOr<std::string> server_final =
        server_.OnClientFinal(line.substr(kAuthContinuePrefix.size()));
    if (!server_final.ok()) {
        // Deliberately the same refusal as a malformed line: wrong
        // password and unknown user already collapsed inside
        // scram::Server, and this collapses the rest.
        return Refuse();
    }
    return {"AUTH+ " + server_final.value(), /*authenticated=*/true, /*close=*/false};
}

}  // namespace kds::server
