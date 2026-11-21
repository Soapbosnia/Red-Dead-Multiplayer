/*
 * Server browser queries: see server_query.h. Each request runs on its own short-lived thread; results are handed to
 * the main thread through a mutex-guarded queue.
 */

#include <utils/safe_win32.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <integrations/client/masterlist_browser.h>

#include "client/server_query.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

namespace RDMP::ServerQuery {
    namespace {
        std::mutex g_mutex;
        std::vector<Info> g_results;
        std::deque<std::pair<std::string, std::string>> g_errors;
        std::atomic<int> g_running {0};
        std::atomic<bool> g_shutdown {false};

        void Push(Info info) {
            std::lock_guard lock(g_mutex);
            g_results.push_back(std::move(info));
        }

        // GET /rdmp, else the Framework's GET / (an older or foreign server: no name or player count).
        Info Ask(const std::string &list, const Address &address) {
            Info info;
            info.list = list;
            info.host = address.host;
            info.port = address.port;
            httplib::Client client(address.host, address.port);
            client.set_connection_timeout(1, 500000);
            client.set_read_timeout(2, 0);
            const auto start = std::chrono::steady_clock::now();
            auto res         = client.Get("/rdmp");
            info.pingMs      = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
            if (res && res->status == 200) {
                const auto doc = nlohmann::json::parse(res->body, nullptr, false);
                if (doc.is_object()) {
                    info.online     = true;
                    info.name       = doc.value("name", std::string());
                    info.version    = doc.value("version", std::string());
                    info.players    = doc.value("players", 0);
                    info.maxPlayers = doc.value("maxPlayers", 0);
                    info.password   = doc.value("password", false);
                }
                return info;
            }
            if (res = client.Get("/"); res && res->status == 200) {
                const auto doc = nlohmann::json::parse(res->body, nullptr, false);
                if (doc.is_object() && doc.value("mod_slug", std::string()) == "rdmp") {
                    info.online     = true;
                    info.name       = address.host + ":" + std::to_string(address.port);
                    info.version    = doc.value("mod_version", std::string());
                    info.maxPlayers = doc.value("max_players", 0);
                    info.password   = doc.value("password_required", false);
                }
            }
            if (!info.online) {
                info.pingMs = -1;
            }
            return info;
        }

        template <typename Fn>
        void Spawn(Fn &&fn) {
            if (g_shutdown.load()) {
                return;
            }
            ++g_running;
            std::thread([fn = std::forward<Fn>(fn)]() mutable {
                fn();
                --g_running;
            }).detach();
        }
    } // namespace

    void QueryAddresses(const std::string &list, const std::vector<Address> &addresses) {
        for (const auto &address : addresses) {
            Spawn([list, address] {
                Push(Ask(list, address));
            });
        }
    }

    void QueryMasterlist(const std::string &masterlistUrl, const std::string &gameMode) {
        Spawn([masterlistUrl, gameMode] {
            Framework::Integrations::Client::MasterlistBrowser browser;
            std::vector<Framework::Integrations::Client::MasterlistServer> servers;
            bool done = false;
            if (browser.Init(masterlistUrl, gameMode)) {
                browser.Refresh();
                for (int i = 0; i < 100 && !g_shutdown.load(); ++i) { // up to 10 s
                    if (browser.Poll(servers)) {
                        done = true;
                        break;
                    }
                    if (browser.State() == Framework::Integrations::Client::MasterlistBrowser::Status::Failed) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            const std::string error = done ? std::string() : (browser.LastError().empty() ? "the masterlist did not answer" : browser.LastError());
            browser.Shutdown();
            if (!done) {
                std::lock_guard lock(g_mutex);
                g_errors.emplace_back("internet", error);
                return;
            }
            std::vector<Address> addresses;
            for (const auto &s : servers) {
                addresses.push_back({s.host, s.port});
            }
            if (addresses.empty()) {
                std::lock_guard lock(g_mutex);
                g_errors.emplace_back("internet", "no servers are listed right now");
                return;
            }
            QueryAddresses("internet", addresses);
        });
    }

    std::vector<Info> Poll() {
        std::lock_guard lock(g_mutex);
        std::vector<Info> out;
        out.swap(g_results);
        return out;
    }

    bool PollError(std::string &list, std::string &message) {
        std::lock_guard lock(g_mutex);
        if (g_errors.empty()) {
            return false;
        }
        list    = g_errors.front().first;
        message = g_errors.front().second;
        g_errors.pop_front();
        return true;
    }

    void Shutdown() {
        g_shutdown = true;
        for (int i = 0; i < 50 && g_running.load() > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
} // namespace RDMP::ServerQuery
