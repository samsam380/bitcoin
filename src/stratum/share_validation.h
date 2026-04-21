// Copyright (c) 2026 The Bitcoin Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_STRATUM_SHARE_VALIDATION_H
#define BITCOIN_STRATUM_SHARE_VALIDATION_H

#include <stratum/session.h>
#include <stratum/stratum_messages.h>

#include <arith_uint256.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <optional>
#include <string>

namespace stratum {

struct Job;

struct ShareValidationResult {
    bool accepted_share{false};
    bool accepted_block{false};
    std::string reject_reason;
    uint256 block_hash;
    uint32_t ntime{0};
    uint32_t nonce{0};
    uint32_t version{0};
    std::optional<CTransactionRef> coinbase;
};

ShareValidationResult ValidateShare(const SubmitRequest& req, const Session& session, const Job& job, const arith_uint256& share_target);

} // namespace stratum

#endif // BITCOIN_STRATUM_SHARE_VALIDATION_H
