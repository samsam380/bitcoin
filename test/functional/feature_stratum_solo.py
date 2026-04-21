#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Knots developers
# Distributed under the MIT software license.
"""Embedded Stratum solo end-to-end on regtest."""

import hashlib
import json
import socket
import struct

from test_framework.test_framework import BitcoinTestFramework


def dbl_sha(b: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def uint256_from_compact(nbits: int) -> int:
    exponent = nbits >> 24
    mantissa = nbits & 0x007fffff
    if exponent <= 3:
        return mantissa >> (8 * (3 - exponent))
    return mantissa << (8 * (exponent - 3))


class StratumSoloTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def mine_via_stratum(self, host: str, port: int):
        s = socket.create_connection((host, port), timeout=10)
        s.settimeout(10)
        rf = s.makefile('rwb', buffering=0)

        def send(obj):
            rf.write((json.dumps(obj) + "\n").encode())

        def recv():
            return json.loads(rf.readline().decode())

        send({"id": 1, "method": "mining.subscribe", "params": []})
        sub = recv()
        extranonce1 = sub["result"][1]
        extranonce2_size = sub["result"][2]

        send({"id": 2, "method": "mining.authorize", "params": ["worker", "x"]})
        auth = recv()
        assert auth["result"] is True

        msg1 = recv()
        msg2 = recv()
        notify = msg1 if msg1.get("method") == "mining.notify" else msg2
        if notify.get("method") != "mining.notify":
            notify = recv()

        params = notify["params"]
        job_id, prevhash, coinb1, coinb2, branches, version_hex, nbits_hex, ntime_hex, _ = params

        extranonce2 = "00" * extranonce2_size
        coinbase = bytes.fromhex(coinb1 + extranonce1 + extranonce2 + coinb2)
        coinbase_hash = dbl_sha(coinbase)

        merkle = coinbase_hash
        for b in branches:
            merkle = dbl_sha(merkle + bytes.fromhex(b))

        version = int(version_hex, 16)
        nbits = int(nbits_hex, 16)
        ntime = int(ntime_hex, 16)
        target = uint256_from_compact(nbits)

        prevhash_le = bytes.fromhex(prevhash)[::-1]
        merkle_le = merkle[::-1]

        found_nonce = None
        for nonce in range(0, 0x1000000):
            header = struct.pack('<I', version) + prevhash_le + merkle_le + struct.pack('<III', ntime, nbits, nonce)
            h = int.from_bytes(dbl_sha(header)[::-1], 'big')
            if h <= target:
                found_nonce = nonce
                break

        assert found_nonce is not None

        send({
            "id": 3,
            "method": "mining.submit",
            "params": ["worker", job_id, extranonce2, f"{ntime:08x}", f"{found_nonce:08x}"]
        })
        submit = recv()
        assert submit["result"] is True

        s.close()

    def run_test(self):
        addr = self.nodes[0].getnewaddress()
        self.restart_node(0, extra_args=[
            '-regtest=1',
            '-server=1',
            '-stratum=1',
            '-stratumbind=127.0.0.1',
            '-stratumport=3333',
            '-stratumdifficulty=1',
            f'-stratumpayoutaddress={addr}',
        ])

        start_height = self.nodes[0].getblockcount()
        self.mine_via_stratum('127.0.0.1', 3333)
        self.wait_until(lambda: self.nodes[0].getblockcount() > start_height)

        info = self.nodes[0].getstratuminfo()
        assert info['enabled']
        assert info['accepted_shares'] >= 1


if __name__ == '__main__':
    StratumSoloTest(__file__).main()
