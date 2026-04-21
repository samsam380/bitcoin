// Copyright (c) 2026 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <stratum/jobmanager.h>

#include <common/hex.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <algorithm>
#include <streams.h>
#include <tinyformat.h>
#include <util/strencodings.h>

namespace stratum {

JobManager::JobManager(TemplateProvider& template_provider, uint32_t extranonce2_size, const std::string& payout_address)
    : m_template_provider(template_provider), m_extranonce2_size(extranonce2_size), m_payout_address(payout_address)
{
}

std::string JobManager::NewJobId()
{
    return strprintf("%08x", m_next_job_id++);
}

std::pair<std::string, std::string> JobManager::BuildCoinbaseSplit(const CTransaction& coinbase) const
{
    CMutableTransaction cb{coinbase};
    if (cb.vin.empty()) return {"", ""};

    const size_t marker_size = 4 + m_extranonce2_size;
    std::vector<unsigned char> marker(marker_size);
    for (size_t i = 0; i < marker_size; ++i) marker[i] = static_cast<unsigned char>(0xf0 + (i & 0x0f));

    cb.vin[0].scriptSig.insert(cb.vin[0].scriptSig.end(), marker.begin(), marker.end());
    CDataStream ss_tx(SER_NETWORK, PROTOCOL_VERSION);
    ss_tx << TX_WITH_WITNESS(CTransaction{cb});
    const std::vector<unsigned char> bytes{ss_tx.begin(), ss_tx.end()};

    const auto it = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    if (it == bytes.end()) return {HexStr(bytes), ""};

    const size_t pos = it - bytes.begin();
    const std::vector<unsigned char> b1(bytes.begin(), bytes.begin() + pos);
    const std::vector<unsigned char> b2(bytes.begin() + pos + marker.size(), bytes.end());
    return {HexStr(b1), HexStr(b2)};
}

std::optional<Job> JobManager::RefreshJobs(RefreshReason reason)
{
    auto tpl = m_template_provider.Refresh(reason);
    if (!tpl) return std::nullopt;

    Job job;
    job.prevhash = uint256S(tpl->prevhash);
    const auto [coinb1, coinb2] = BuildCoinbaseSplit(*tpl->block.vtx.at(0));
    job.coinb1 = coinb1;
    job.coinb2 = coinb2;
    job.merkle_branches = tpl->merkle_branch;
    job.version = tpl->version;
    job.nbits = tpl->nbits;
    job.ntime = tpl->ntime;
    job.clean_jobs = tpl->clean_jobs;
    job.height = tpl->height;
    job.block = tpl->block;
    job.block_template = tpl->block_template;

    LOCK(m_mutex);
    job.id = NewJobId();
    m_current_job = job;
    m_jobs[job.id] = job;
    if (m_jobs.size() > 32) m_jobs.erase(m_jobs.begin());
    return m_current_job;
}

std::optional<Job> JobManager::CurrentJob() const
{
    LOCK(m_mutex);
    return m_current_job;
}

std::optional<Job> JobManager::CreateJobForSession(uint64_t session_id) const
{
    LOCK(m_mutex);
    if (!m_current_job.has_value()) return std::nullopt;
    if (!m_extranonce1.contains(session_id)) return std::nullopt;
    return m_current_job;
}

std::optional<Job> JobManager::GetJob(const std::string& job_id) const
{
    LOCK(m_mutex);
    if (const auto it = m_jobs.find(job_id); it != m_jobs.end()) return it->second;
    return std::nullopt;
}

std::string JobManager::GetSessionExtranonce1(uint64_t session_id)
{
    LOCK(m_mutex);
    if (!m_extranonce1.contains(session_id)) {
        m_extranonce1.emplace(session_id, strprintf("%08x", session_id));
    }
    return m_extranonce1.at(session_id);
}

UniValue JobManager::BuildNotify(const Job& job) const
{
    UniValue params(UniValue::VARR);
    params.push_back(job.id);
    params.push_back(job.prevhash.GetHex());
    params.push_back(job.coinb1);
    params.push_back(job.coinb2);

    UniValue branches(UniValue::VARR);
    for (const auto& branch : job.merkle_branches) branches.push_back(branch.GetHex());
    params.push_back(std::move(branches));

    params.push_back(strprintf("%08x", job.version));
    params.push_back(strprintf("%08x", job.nbits));
    params.push_back(strprintf("%08x", job.ntime));
    params.push_back(job.clean_jobs);

    UniValue payload(UniValue::VOBJ);
    payload.pushKV("id", UniValue{UniValue::VNULL});
    payload.pushKV("method", "mining.notify");
    payload.pushKV("params", std::move(params));
    return payload;
}

} // namespace stratum
