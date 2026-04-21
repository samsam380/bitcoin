# Embedded Stratum V1 SOLO mining (MVP)

## Start a regtest node with Stratum

```bash
bitcoind \
  -regtest=1 \
  -server=1 \
  -rpcuser=u -rpcpassword=p \
  -stratum=1 \
  -stratumbind=127.0.0.1 \
  -stratumport=3333 \
  -stratumdifficulty=1 \
  -stratumpayoutaddress=<regtest-address>
```

## Verify operation

```bash
bitcoin-cli -regtest -rpcuser=u -rpcpassword=p getstratuminfo
ss -ltnp | grep 3333
```

Expected:
- `getstratuminfo.enabled=true`
- `clients` increases when miners connect
- `accepted_shares` and `blocks_found` increment while mining

## Connect a Stratum v1 miner

Point miner to:
- URL/Host: `127.0.0.1`
- Port: `3333`
- User: any non-empty worker name (solo MVP)
- Password: arbitrary (currently ignored)

## Manual regtest verification flow

1. Start node with flags above.
2. From Python or a Stratum client, send `mining.subscribe` then `mining.authorize`.
3. Receive `mining.set_difficulty` and `mining.notify`.
4. Solve the notify job against the advertised `nbits` and submit using `mining.submit`.
5. Confirm chain height increased:

```bash
bitcoin-cli -regtest -rpcuser=u -rpcpassword=p getblockcount
bitcoin-cli -regtest -rpcuser=u -rpcpassword=p getstratuminfo
```

## Future work (post-MVP)

- Vardiff
- Stratum V2 / job-declaration
- DATUM-like federation/external template hooks
- Pooled payout accounting and persistent share storage
