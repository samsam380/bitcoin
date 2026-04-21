// Copyright (c) 2026 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <stratum/server.h>

#include <common/args.h>
#include <interfaces/mining.h>
#include <logging.h>
#include <netbase.h>
#include <stratum/share_validation.h>
#include <stratum/stratum_messages.h>
#include <util/sock.h>
#include <util/thread.h>

#include <chrono>

namespace stratum {
namespace {
uint32_t ParseHexU32(const std::string& s, uint32_t def)
{
    try {
        size_t idx{0};
        const uint32_t out = static_cast<uint32_t>(std::stoul(s, &idx, 16));
        return idx == s.size() ? out : def;
    } catch (...) {
        return def;
    }
}

UniValue BuildSetDifficulty(double difficulty)
{
    UniValue params(UniValue::VARR);
    params.push_back(difficulty);

    UniValue payload(UniValue::VOBJ);
    payload.pushKV("id", UniValue{UniValue::VNULL});
    payload.pushKV("method", "mining.set_difficulty");
    payload.pushKV("params", std::move(params));
    return payload;
}
} // namespace

Server::Server(const Config& config, interfaces::Mining& mining)
    : m_config(config), m_template_provider(mining), m_job_manager(m_template_provider, m_config.extranonce2_size, m_config.payout_address)
{
}

Server::~Server()
{
    Stop();
}

bool Server::Start()
{
    if (!m_config.enabled) return true;
    if (m_running.exchange(true)) return true;

    const auto bind_addr = Lookup(m_config.bind, m_config.port, /*fAllowLookup=*/false);
    if (!bind_addr.has_value()) {
        LogPrintf("Stratum bind lookup failed for %s:%u\n", m_config.bind, m_config.port);
        m_running = false;
        return false;
    }

    auto listen_sock = CreateSock(bind_addr->GetSAFamily(), SOCK_STREAM, IPPROTO_TCP);
    if (!listen_sock) {
        LogPrintf("Stratum create socket failed\n");
        m_running = false;
        return false;
    }

    int one = 1;
    listen_sock->SetSockOpt(SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (!bind_addr->GetSockAddr(reinterpret_cast<sockaddr*>(&addr), &addr_len) ||
        listen_sock->Bind(reinterpret_cast<sockaddr*>(&addr), addr_len) != 0 ||
        listen_sock->Listen(16) != 0) {
        LogPrintf("Stratum bind/listen failed on %s:%u\n", m_config.bind, m_config.port);
        m_running = false;
        return false;
    }

    m_listen_sock = std::shared_ptr<Sock>(std::move(listen_sock));
    m_job_manager.RefreshJobs(RefreshReason::NEW_PREVHASH);
    m_listener_thread = std::thread(&util::TraceThread, "stratum-listener", [this] { ListenerThread(); });
    m_refresh_thread = std::thread(&util::TraceThread, "stratum-refresh", [this] { ThreadRun(); });
    LogPrintf("Stratum enabled on %s:%u\n", m_config.bind, m_config.port);
    return true;
}

void Server::Interrupt()
{
    m_running.store(false);
    if (m_listen_sock) m_listen_sock->Close();
}

void Server::Stop()
{
    Interrupt();
    if (m_listener_thread.joinable()) m_listener_thread.join();
    if (m_refresh_thread.joinable()) m_refresh_thread.join();

    std::vector<std::thread> join_threads;
    {
        LOCK(m_mutex);
        for (auto& [_, client] : m_clients) {
            if (client && client->sock) client->sock->Close();
            if (client && client->thread.joinable()) join_threads.push_back(std::move(client->thread));
        }
        m_clients.clear();
        m_sessions.clear();
    }
    for (auto& t : join_threads) t.join();
}

Session& Server::GetOrCreateSession(uint64_t session_id)
{
    if (!m_sessions.contains(session_id)) {
        auto s = std::make_unique<Session>();
        s->session_id = session_id;
        s->extranonce2_size = m_config.extranonce2_size;
        s->difficulty = m_config.difficulty;
        s->version_rolling_mask = m_config.version_rolling ? m_config.version_rolling_mask : 0;
        s->extranonce1 = m_job_manager.GetSessionExtranonce1(session_id);
        m_sessions.emplace(session_id, std::move(s));
    }
    return *m_sessions.at(session_id);
}

void Server::RemoveSession(uint64_t session_id)
{
    LOCK(m_mutex);
    m_sessions.erase(session_id);
    if (auto it = m_clients.find(session_id); it != m_clients.end()) {
        if (it->second->thread.joinable() && it->second->thread.get_id() == std::this_thread::get_id()) {
            it->second->thread.detach();
        }
        m_clients.erase(it);
    }
}

bool Server::SendJson(uint64_t session_id, const UniValue& obj)
{
    std::shared_ptr<Sock> sock;
    Mutex* send_mutex{nullptr};
    {
        LOCK(m_mutex);
        if (!m_clients.contains(session_id)) return false;
        sock = m_clients.at(session_id)->sock;
        send_mutex = &m_clients.at(session_id)->send_mutex;
    }

    const std::string payload = obj.write() + "\n";
    LOCK(*send_mutex);
    size_t sent{0};
    while (sent < payload.size()) {
        const ssize_t ret = sock->Send(payload.data() + sent, payload.size() - sent, 0);
        if (ret <= 0) return false;
        sent += static_cast<size_t>(ret);
    }
    return true;
}

void Server::SendInitialMessages(uint64_t session_id)
{
    std::optional<Job> job;
    {
        LOCK(m_mutex);
        if (!m_sessions.contains(session_id)) return;
        Session& s = *m_sessions.at(session_id);
        if (!(s.subscribed && s.authorized) || s.sent_initial_notify) return;
        s.sent_initial_notify = true;
        job = m_job_manager.CreateJobForSession(session_id);
    }
    if (!job.has_value()) return;
    SendJson(session_id, BuildSetDifficulty(m_config.difficulty));
    SendJson(session_id, m_job_manager.BuildNotify(*job));
}

UniValue Server::HandleMessage(uint64_t session_id, const UniValue& request)
{
    const UniValue id = request.exists("id") ? request["id"] : UniValue{UniValue::VNULL};
    if (!request.isObject() || !request.exists("method")) {
        return BuildError(id, 20, "malformed-request");
    }

    const std::string method = request["method"].get_str();
    const UniValue params = request.exists("params") ? request["params"] : UniValue{UniValue::VARR};

    {
        LOCK(m_mutex);
        GetOrCreateSession(session_id);
    }

    if (method == "mining.subscribe") {
        std::string extranonce1;
        uint32_t extranonce2_size{0};
        {
            LOCK(m_mutex);
            Session& session = *m_sessions.at(session_id);
            session.subscribed = true;
            extranonce1 = session.extranonce1;
            extranonce2_size = session.extranonce2_size;
        }

        UniValue subscriptions(UniValue::VARR);
        UniValue s1(UniValue::VARR);
        s1.push_back("mining.set_difficulty");
        s1.push_back("subid-diff");
        subscriptions.push_back(std::move(s1));
        UniValue s2(UniValue::VARR);
        s2.push_back("mining.notify");
        s2.push_back("subid-notify");
        subscriptions.push_back(std::move(s2));

        UniValue result(UniValue::VARR);
        result.push_back(std::move(subscriptions));
        result.push_back(extranonce1);
        result.push_back(extranonce2_size);

        UniValue resp(UniValue::VOBJ);
        resp.pushKV("id", id);
        resp.pushKV("result", std::move(result));
        resp.pushKV("error", UniValue{UniValue::VNULL});
        return resp;
    }

    if (method == "mining.authorize") {
        if (!params.isArray() || params.size() < 1 || params[0].get_str().empty()) {
            return BuildError(id, 24, "invalid-worker-name");
        }
        {
            LOCK(m_mutex);
            Session& session = *m_sessions.at(session_id);
            session.worker_name = params[0].get_str();
            session.authorized = true;
        }
        return BuildSuccess(id);
    }

    if (method == "mining.suggest_difficulty" || method == "mining.extranonce.subscribe") {
        return BuildSuccess(id);
    }

    if (method == "mining.submit") {
        auto submit = ParseSubmitParams(params);
        if (!submit) return BuildError(id, 20, "invalid-submit-format");

        auto job = m_job_manager.GetJob(submit->job_id);
        auto current = m_job_manager.CurrentJob();
        if (!job) {
            LOCK(m_mutex);
            m_rejected_shares++;
            if (m_sessions.contains(session_id)) m_sessions.at(session_id)->rejected++;
            return BuildError(id, 21, "job-not-found");
        }

        if (current.has_value() && current->prevhash != job->prevhash) {
            LOCK(m_mutex);
            m_rejected_shares++;
            if (m_sessions.contains(session_id)) m_sessions.at(session_id)->stale++;
            return BuildError(id, 21, "stale-share");
        }

        Session session;
        {
            LOCK(m_mutex);
            if (!m_sessions.contains(session_id)) return BuildError(id, 25, "unknown-session");
            session = *m_sessions.at(session_id);
        }

        arith_uint256 share_target;
        share_target.SetCompact(job->nbits);
        if (session.difficulty > 1.0) share_target /= static_cast<uint32_t>(session.difficulty);

        const auto val = ValidateShare(*submit, session, *job, share_target);
        if (!val.accepted_share) {
            LOCK(m_mutex);
            m_rejected_shares++;
            if (m_sessions.contains(session_id)) m_sessions.at(session_id)->rejected++;
            return BuildError(id, 23, val.reject_reason);
        }

        bool block_submitted{false};
        if (val.accepted_block && val.coinbase.has_value() && job->block_template) {
            block_submitted = job->block_template->submitSolution(val.version, val.ntime, val.nonce, *val.coinbase);
        }

        {
            LOCK(m_mutex);
            m_accepted_shares++;
            if (m_sessions.contains(session_id)) m_sessions.at(session_id)->accepted++;
            if (block_submitted) m_blocks_found++;
        }
        return BuildSuccess(id, true);
    }

    return BuildError(id, 404, "method-not-found");
}

void Server::BroadcastNotify(const Job& job, bool send_set_difficulty)
{
    std::vector<uint64_t> sessions;
    {
        LOCK(m_mutex);
        for (const auto& [sid, session] : m_sessions) {
            if (session->subscribed && session->authorized && m_clients.contains(sid)) sessions.push_back(sid);
        }
    }

    for (const auto sid : sessions) {
        if (send_set_difficulty) SendJson(sid, BuildSetDifficulty(m_config.difficulty));
        SendJson(sid, m_job_manager.BuildNotify(job));
    }
}

void Server::ListenerThread()
{
    while (m_running.load()) {
        Sock::Event occurred{0};
        if (!m_listen_sock->Wait(std::chrono::milliseconds{200}, Sock::RECV, &occurred)) continue;
        if (!(occurred & Sock::RECV)) continue;

        sockaddr_storage addr{};
        socklen_t addr_len = sizeof(addr);
        auto sock = m_listen_sock->Accept(reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (!sock) continue;

        uint64_t session_id;
        std::shared_ptr<Sock> client_sock{std::move(sock)};
        {
            LOCK(m_mutex);
            session_id = m_next_session_id++;
            auto conn = std::make_unique<ClientConn>();
            conn->sock = client_sock;
            conn->thread = std::thread(&util::TraceThread, "stratum-client", [this, session_id, client_sock] {
                ClientThread(session_id, client_sock);
            });
            m_clients.emplace(session_id, std::move(conn));
            GetOrCreateSession(session_id);
        }
    }
}

void Server::ClientThread(uint64_t session_id, std::shared_ptr<Sock> sock)
{
    std::string buffered;
    char buf[4096];

    while (m_running.load()) {
        const ssize_t n = sock->Recv(buf, sizeof(buf), 0);
        if (n <= 0) break;
        buffered.append(buf, buf + n);

        size_t pos;
        while ((pos = buffered.find('\n')) != std::string::npos) {
            std::string line = buffered.substr(0, pos);
            buffered.erase(0, pos + 1);
            if (line.empty()) continue;

            UniValue request(UniValue::VOBJ);
            if (!request.read(line)) {
                SendJson(session_id, BuildError(UniValue{UniValue::VNULL}, 20, "invalid-json"));
                continue;
            }

            const std::string method = request.exists("method") ? request["method"].get_str() : "";
            const UniValue response = HandleMessage(session_id, request);
            SendJson(session_id, response);

            if (method == "mining.subscribe" || method == "mining.authorize") {
                SendInitialMessages(session_id);
            }
        }
    }

    RemoveSession(session_id);
}

void Server::ThreadRun()
{
    while (m_running.load()) {
        const auto current = m_job_manager.CurrentJob();
        auto refreshed = m_job_manager.RefreshJobs(RefreshReason::TEMPLATE_UPDATE_ONLY);
        if (current.has_value() && refreshed.has_value() && refreshed->prevhash != current->prevhash) {
            refreshed = m_job_manager.RefreshJobs(RefreshReason::NEW_PREVHASH);
        }

        if (refreshed.has_value() && (!current.has_value() || refreshed->id != current->id)) {
            BroadcastNotify(*refreshed, /*send_set_difficulty=*/false);
        }
        UninterruptibleSleep(std::chrono::milliseconds{m_config.job_refresh_ms});
    }
}

Info Server::GetInfo() const
{
    Info info;
    info.enabled = m_config.enabled;
    info.bind = m_config.bind;
    info.port = m_config.port;
    info.version_rolling_enabled = m_config.version_rolling;
    info.version_rolling_mask = m_config.version_rolling_mask;

    const auto current = m_job_manager.CurrentJob();
    if (current.has_value()) {
        info.current_job_id = current->id;
        info.current_height = current->height;
        info.current_prevhash = current->prevhash.GetHex();
    }

    LOCK(m_mutex);
    info.clients = m_clients.size();
    info.accepted_shares = m_accepted_shares;
    info.rejected_shares = m_rejected_shares;
    info.blocks_found = m_blocks_found;
    return info;
}

Config GetConfig(const ArgsManager& args, bool is_regtest)
{
    Config cfg;
    cfg.enabled = args.GetBoolArg("-stratum", false);
    cfg.bind = args.GetArg("-stratumbind", "127.0.0.1");
    cfg.port = static_cast<uint16_t>(args.GetIntArg("-stratumport", 3333));
    cfg.extranonce2_size = static_cast<uint32_t>(args.GetIntArg("-stratumextranonce2size", 4));
    cfg.difficulty = args.GetIntArg("-stratumdifficulty", is_regtest ? 1 : 1024);
    cfg.payout_address = args.GetArg("-stratumpayoutaddress", "");
    cfg.version_rolling = args.GetBoolArg("-stratumversionrolling", false);
    cfg.version_rolling_mask = ParseHexU32(args.GetArg("-stratumversionrollingmask", "1fffe000"), 0x1fffe000);
    cfg.job_refresh_ms = args.GetIntArg("-stratumjobrefreshms", 1000);
    cfg.allow_self_select = args.GetBoolArg("-stratumallowselfselect", false);
    return cfg;
}

} // namespace stratum
