Bitcoin Knots — Node-Native Solo Stratum Fork (Tested and working on mainnet!)
============================================

This repository is an experimental fork of [Bitcoin Knots](https://bitcoinknots.org) with an embedded solo Stratum V1 mining server built directly into the Bitcoin node.

The goal is simple:

> Let a miner such as a Bitaxe or ASIC connect directly to your own Bitcoin node and receive mining jobs without requiring Miningcore, ckpool, a public solo pool, or any external Stratum proxy.

In other words:

```text
ASIC / Bitaxe → Bitcoin node → block template → submitted block
```

No external mining pool server. No third-party block template provider. No RPC-to-self bridge. The node itself serves the mining job.

This project is experimental and intended for research, testing, and development.

Current status
--------------

This fork currently includes a node-native embedded Stratum V1 solo-mining server that can be enabled with startup flags.

Confirmed working so far:

- Built from source on Raspberry Pi / Linux server
- Embedded Stratum listener binding on `127.0.0.1`
- Embedded Stratum listener binding on `0.0.0.0`
- Remote LAN / internet connection from a Bitaxe miner
- `mining.configure`
- `mining.subscribe`
- `mining.authorize`
- `mining.set_difficulty`
- `mining.notify`
- `mining.submit`
- Version rolling support for Bitaxe / ESP-Miner
- Stratum extranonce handling
- Coinbase reconstruction from `coinb1 + extranonce1 + extranonce2 + coinb2`
- Non-witness coinbase transaction hashing for txid / merkle validation
- Correct Stratum byte-order handling for Bitaxe / ESP-Miner
- Accepted shares on regtest
- Regtest blocks found through the embedded Stratum path
- Mainnet Bitaxe BM1370 direct-to-node test
- Diff-1 mainnet shares accepted from Bitaxe
- No low-difficulty rejects after the Bitaxe prevhash compatibility fix
- Real block candidate submission path uses the reconstructed miner coinbase
- Functional test coverage for the embedded Stratum solo flow

Mainnet live test status:

- A real Bitaxe BM1370 was connected directly to the node using Stratum V1.
- The node accepted Bitaxe `mining.submit` shares at difficulty 1.
- The previous `low-difficulty-share` rejection issue was fixed.
- Share validation now agrees with Bitaxe's reported candidate difficulty.
- The real-block submission path now submits the reconstructed miner coinbase rather than the original template coinbase.

No mainnet block has been found with this fork at the time of writing. Accepted shares prove Stratum job compatibility and share validation; they do not by themselves prove a real Bitcoin block has been mined.

Why this exists
---------------

Bitcoin nodes already validate blocks, track the chain tip, maintain mempool policy, and can construct block templates.

However, most ASIC miners do not speak directly to Bitcoin Core or Bitcoin Knots. They usually expect a Stratum server. In practice, this means a solo miner often needs extra software between the miner and their own node, such as:

- Miningcore
- ckpool
- public solo pools
- custom Stratum proxy software

This fork explores a more direct model: the Bitcoin node itself exposes the mining interface that the miner expects.

The long-term idea is simple: if you are solo mining, your hasher should be able to talk directly to your node.

Important warning
-----------------

This is experimental software.

Do not use this on mainnet with meaningful funds or serious hashpower unless you fully understand the risks and have reviewed the code yourself.

Potential risks include:

- invalid block template handling
- stale jobs
- incorrect share validation
- Stratum compatibility issues
- miner-specific behavior differences
- denial-of-service surface from exposing a Stratum port
- bugs in block submission logic
- incomplete production hardening
- missing production-grade monitoring

If you expose a Stratum port publicly, use firewall rules and understand that you are exposing an additional network service from your node.

Embedded Stratum server
-----------------------

The embedded Stratum server is controlled with startup flags.

Example mainnet startup:

```bash
./bin/bitcoind \
  -server=1 \
  -stratum=1 \
  -stratumbind=0.0.0.0 \
  -stratumport=3338 \
  -stratumdifficulty=1 \
  -stratumversionrolling=1 \
  -stratumversionrollingmask=1fffe000 \
  -stratumpayoutaddress="<BITCOIN_ADDRESS>"
```

Example regtest startup:

```bash
./bin/bitcoind -regtest \
  -server=1 \
  -fallbackfee=0.0001 \
  -stratum=1 \
  -stratumbind=0.0.0.0 \
  -stratumport=3333 \
  -stratumdifficulty=1 \
  -rpcuser=bitcoin \
  -rpcpassword=bitcoinpass \
  -stratumpayoutaddress="<REGTEST_ADDRESS>" \
  -printtoconsole
```

Example Bitaxe / ASIC miner settings:

```text
Pool URL: stratum+tcp://<node-ip-or-domain>:3338
Worker:   miner1
Password: x
```

For Bitaxe / ESP-Miner version rolling, the tested mask is:

```text
1fffe000
```

Useful RPC
----------

This fork adds:

```bash
bitcoin-cli getstratuminfo
```

Example output:

```json
{
  "enabled": true,
  "listening": true,
  "accept_loop_running": true,
  "bind": "0.0.0.0",
  "port": 3333,
  "clients": 1,
  "connected_clients": 1,
  "authorized_clients": 1,
  "current_job_id": "00000037",
  "current_height": 2,
  "current_prevhash": "...",
  "accepted_shares": 19,
  "rejected_shares": 0,
  "blocks_found": 0,
  "last_client_ip": "192.168.1.7",
  "last_authorized_worker": "miner1",
  "last_accepted_share_hash": "...",
  "last_rejected_share_reason": "...",
  "last_block_submission_result": "...",
  "uptime": 123,
  "last_notify_time": 1760000000,
  "version_rolling_enabled": true,
  "version_rolling_mask": "1fffe000"
}
```

Development notes
-----------------

The embedded Stratum implementation currently includes:

- TCP listener
- per-client session handling
- line-delimited JSON-RPC Stratum messages
- `mining.configure`
- `mining.subscribe`
- `mining.authorize`
- `mining.submit`
- `mining.suggest_difficulty`
- `mining.extranonce.subscribe`
- job creation from internal mining/template interfaces
- share reconstruction and validation
- version rolling handling
- Stratum prevhash / merkle formatting compatible with Bitaxe / ESP-Miner
- block candidate submission path
- reconstructed miner coinbase submission for real block candidates
- observability through `getstratuminfo`
- functional test coverage

This is currently solo-only. It is not a pooled accounting system. There are no user balances, payouts, pool accounting, worker reward splits, or pool operator features.

No Miningcore, ckpool, or external Stratum proxy is required.

Testing
-------

Run the functional Stratum test with descriptor wallets enabled:

```bash
./test/functional/test_runner.py --descriptors feature_stratum_solo.py
```

The regtest flow has been manually tested with a real Bitaxe connected over LAN.

The mainnet Stratum share path has also been tested with a real Bitaxe BM1370 connected directly to the node.

Suggested testing progression:

```text
regtest → signet → small-hashrate mainnet → extended mainnet testing
```

Mainnet testing should be treated carefully. A successful diff-1 share test is not the same thing as a mined block.

If you would like to support my work, like testing this node with high hashrate to see whether actual blocks can be found and mined, we would need to rent several EH/s of hashrate to even have a chance, that needs a lot of money or sats to rent EH/s hashare and leave it running for at least a few weeks.

Bitcoin donation accepted here: bc1qkqx4rs6ccyl3p5th6gavwa7rsudvvzuqgpdp39


Upstream Bitcoin Knots
----------------------

This repository is based on Bitcoin Knots.

For an immediately usable, binary version of the upstream Bitcoin Knots software, see:

https://bitcoinknots.org

What is Bitcoin Knots?
----------------------

Bitcoin Knots connects to the Bitcoin peer-to-peer network to download and fully validate blocks and transactions. It also includes a wallet and graphical user interface, which can be optionally built.

Further information about Bitcoin Knots is available in the `doc` folder.

License
-------

Bitcoin Knots is released under the terms of the MIT license. See `COPYING` for more information or see:

https://opensource.org/licenses/MIT

Development Process
-------------------

Development generally takes place as part of Bitcoin Core and is merged into Knots for each release.

Even if your pull request to Core is closed, or if your feature is not suitable for Core, it may still be eligible for inclusion in Bitcoin Knots. In this case, a pull request may be opened on the Knots GitHub for review and consideration.

Testing and code review are critical because this is security-sensitive software.

Translations
------------

Changes to translations as well as new translations can be submitted to Bitcoin Core's Transifex page.

Translations are periodically pulled from Transifex and merged into the git repository. See the translation process for details.

Important: translation changes should not be submitted as GitHub pull requests because the next pull from Transifex would overwrite them.
