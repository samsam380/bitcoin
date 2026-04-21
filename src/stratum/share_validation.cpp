// Copyright (c) 2026 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <stratum/share_validation.h>

#include <common/hex.h>
#include <consensus/merkle.h>
#include <protocol.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <stratum/jobmanager.h>
#include <streams.h>
#include <util/strencodings.h>

namespace stratum {
namespace {

bool ParseHexU32(const std::string& s, uint32_t& out)
{
    if (!IsHex(s) || s.size() > 8) return false;
    try {
        size_t idx{0};
        const auto v = std::stoul(s, &idx, 16);
        if (idx != s.size()) return false;
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

ShareValidationResult ValidateShare(const SubmitRequest& req, const Session& session, const Job& job, const arith_uint256& share_target)
{
    ShareValidationResult ret;
    if (req.extranonce2.size() != session.extranonce2_size * 2) {
        ret.reject_reason = "invalid-extranonce2-size";
        return ret;
    }

    if (!IsHex(req.extranonce2) || !IsHex(req.ntime) || !IsHex(req.nonce)) {
        ret.reject_reason = "invalid-hex-field";
        return ret;
    }

    uint32_t ntime{0};
    uint32_t nonce{0};
    if (!ParseHexU32(req.ntime, ntime) || !ParseHexU32(req.nonce, nonce)) {
        ret.reject_reason = "invalid-nonce-or-ntime";
        return ret;
    }

    if (ntime + 7200 < job.ntime || ntime > job.ntime + 7200) {
        ret.reject_reason = "invalid-ntime-range";
        return ret;
    }

    const auto coinbase_bytes = ParseHex(job.coinb1 + session.extranonce1 + req.extranonce2 + job.coinb2);
    if (coinbase_bytes.empty()) {
        ret.reject_reason = "invalid-coinbase";
        return ret;
    }

    CMutableTransaction mutable_cb;
    try {
        CDataStream ds(coinbase_bytes, SER_NETWORK, PROTOCOL_VERSION);
        ds >> TX_WITH_WITNESS(mutable_cb);
    } catch (...) {
        ret.reject_reason = "invalid-coinbase-decode";
        return ret;
    }

    const CTransactionRef coinbase = MakeTransactionRef(mutable_cb);
    uint256 merkle_root = ComputeMerkleRootFromBranch(coinbase->GetHash(), job.merkle_branches, 0);

    CBlockHeader header;
    header.nVersion = static_cast<int32_t>(job.version);
    header.hashPrevBlock = job.prevhash;
    header.hashMerkleRoot = merkle_root;
    header.nTime = ntime;
    header.nBits = job.nbits;
    header.nNonce = nonce;

    const arith_uint256 hash_val{UintToArith256(header.GetHash())};
    arith_uint256 network_target;
    bool fneg, fov;
    network_target.SetCompact(header.nBits, &fneg, &fov);
    if (fneg || fov || network_target == 0) {
        ret.reject_reason = "invalid-network-target";
        return ret;
    }

    ret.accepted_share = hash_val <= share_target;
    ret.accepted_block = hash_val <= network_target;
    ret.block_hash = header.GetHash();
    ret.ntime = ntime;
    ret.nonce = nonce;
    ret.version = job.version;
    ret.coinbase = coinbase;

    if (!ret.accepted_share) ret.reject_reason = "low-difficulty-share";
    return ret;
}

} // namespace stratum
