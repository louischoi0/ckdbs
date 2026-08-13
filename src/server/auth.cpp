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
        std::size_t space2 =
            space == std::string_view::npos ? std::string_view::npos : line.find(' ', space + 1);
        if (space == std::string_view::npos || space2 == std::string_view::npos) {
            // The two-column shape is the pre-authorization format
            // (2026-08-13, same day - no deployed population to migrate).
            // Named so the operator edits a role in instead of hunting.
            return Status::InvalidArgument(
                origin + ":" + std::to_string(line_no) +
                ": expected '<username> <role> <verifier>' - a line without the role "
                "column is the pre-authorization format; add readonly, readwrite or admin");
        }
        std::string username(line.substr(0, space));
        std::string_view role_text = line.substr(space + 1, space2 - space - 1);
        std::string_view verifier_text = line.substr(space2 + 1);

        if (store.users_.find(username) != store.users_.end()) {
            return Status::InvalidArgument(origin + ":" + std::to_string(line_no) + ": user '" +
                                            username + "' is listed twice");
        }
        auto role = ParseRole(role_text);
        if (!role.ok()) {
            return Status::InvalidArgument(origin + ":" + std::to_string(line_no) + ": " +
                                            role.status().message());
        }
        auto verifier = scram::Verifier::Parse(verifier_text);
        if (!verifier.ok()) {
            return Status::InvalidArgument(origin + ":" + std::to_string(line_no) + ": " +
                                            verifier.status().message());
        }
        store.users_.emplace(std::move(username),
                             UserRecord{std::move(verifier.value()), role.value()});
    }
    return store;
}

StatusOr<UserRecord> FileCredentialStore::Lookup(std::string_view username) const {
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
    : store_(store),
      server_([store](std::string_view user) -> StatusOr<scram::Verifier> {
          // The SCRAM layer proves identity and knows nothing of roles;
          // the record is projected down to what it may see.
          auto record = store->Lookup(user);
          if (!record.ok()) return record.status();
          return std::move(record.value().verifier);
      }) {}

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
    // The proof succeeded, so the user exists and username() is real (it
    // is empty until the exchange is done). A record that vanishes
    // between the two lookups - impossible for the immutable file store,
    // conceivable for a future live one - fails closed as a refusal.
    auto record = store_->Lookup(server_.username());
    if (!record.ok()) return Refuse();
    AuthGate::Result result{"AUTH+ " + server_final.value(), /*authenticated=*/true,
                            /*close=*/false};
    result.username = server_.username();
    result.role = record.value().role;
    return result;
}

}  // namespace kds::server
