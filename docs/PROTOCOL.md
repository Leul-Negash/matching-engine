# Order entry protocol

`ob_server` speaks a line-based text protocol over TCP. Every message is one line
ending in `\n` (a trailing `\r` is ignored), with fields separated by spaces.
Keywords and symbols are case-insensitive, and symbols are upper-cased by the
server. Lines are limited to 256 bytes. Prices are integer ticks and quantities
are integer units.

Because it is plain text you can try it with `nc`:

```
$ nc 127.0.0.1 9000
NEW o1 AAPL BUY 100 15000
ACK o1 1
```

## Client → server

| Message | Meaning |
|---|---|
| `NEW <clordid> <symbol> <BUY\|SELL> <qty> <price\|MKT> [GTC\|IOC\|FOK] [POST]` | New order. A numeric price makes a limit order and `MKT` makes a market order. The time in force defaults to `GTC` for limit orders and `IOC` for market orders; a market order can't be `GTC`. `POST` means post-only: the order is rejected rather than trading on arrival, and it must be a GTC limit order. |
| `CANCEL <clordid>` | Cancel a resting order. |
| `MODIFY <clordid> <price> <qty>` | Change price and/or remaining quantity. Reducing the quantity at the same price keeps the order's queue position; any other change re-queues it at the back and may trade immediately. |
| `BOOK <symbol> [levels]` | Depth snapshot, default 10 levels per side, maximum 1000. |
| `SUB <symbol>` / `UNSUB <symbol>` | Start or stop the public `TRADE` and `BBO` feed for a symbol. |
| `PING` | Liveness check. |

`clordid` is the client's own id for the order: 1–32 characters from
`[A-Za-z0-9_.-]`. It must be unique among the session's live orders, and it can
be reused once the order is finished.

## Server → client

| Message | Meaning |
|---|---|
| `ACK <clordid> <order_id>` | Order accepted. `order_id` is the exchange's id. |
| `REJECT <clordid> <reason>` | Request refused; nothing changed. |
| `FILL <clordid> <price> <qty> <leaves>` | Trade. `leaves` is your remaining open quantity, and `0` means the order is finished. Sent to both sides. |
| `CANCELED <clordid> <qty> <reason>` | `qty` was removed. The reason is `REQUESTED`, `UNFILLED` (IOC or market remainder), `FILL_OR_KILL` or `SELF_TRADE`. |
| `MODIFIED <clordid> <price> <leaves>` | Modify applied. |
| `TRADE <symbol> <price> <qty> <BUY\|SELL>` | Public trade print, giving the aggressor's side. Subscribers only. |
| `BBO <symbol> <bid> <bid_qty> <ask> <ask_qty>` | Top of book changed. `-` marks an empty side. Subscribers only. |
| `LEVEL <symbol> <BID\|ASK> <price> <qty> <orders>` | One snapshot row, best price first. |
| `BOOKEND <symbol>` | End of snapshot. |
| `SUBSCRIBED <symbol>` / `UNSUBSCRIBED <symbol>` | Subscription change confirmed. |
| `PONG` | Reply to `PING`. |
| `ERROR <text>` | Malformed message, unknown symbol in `BOOK`/`SUB`, throttling, or the server is full. |

### Reject reasons

Rejects come either from the gateway or from the matching engine.

**Gateway**
- `UNKNOWN_SYMBOL`: the symbol isn't traded on this server.
- `DUPLICATE_CLORDID`: the session already has a live order with this clordid.
- `UNKNOWN_ORDER`: the clordid isn't a live order of this session. This includes
  an order that was filled just before a cancel arrived.
- `THROTTLED`: the session exceeded its message rate.

**Engine**
- `INVALID_QUANTITY`: zero, larger than the maximum, or not a multiple of the
  lot size.
- `INVALID_PRICE`: out of range or not a multiple of the tick size.
- `INVALID_ORDER_TYPE`: a post-only order that isn't a GTC limit order.
- `POST_ONLY_WOULD_CROSS`
- `UNKNOWN_ORDER`

## Order lifecycle

Every `NEW` gets exactly one `ACK` or `REJECT`. After an `ACK`, the order
produces zero or more `FILL`s and ends with a fill whose `leaves` is 0, with a
`CANCELED`, or by staying open.

```
NEW o1 AAPL BUY 100 15000 IOC        ACK o1 7
                                     FILL o1 14990 60 40
                                     CANCELED o1 40 UNFILLED
```

Engine replies (`ACK`, `FILL`, `CANCELED`, `MODIFIED`, engine rejects) arrive in
the order the engine produced them. Gateway-level replies (`REJECT` for
`UNKNOWN_SYMBOL`, `DUPLICATE_CLORDID`, `UNKNOWN_ORDER` or `THROTTLED`, and
`ERROR`) are sent immediately, so they can overtake engine replies to earlier
messages. Match replies to requests by `clordid`, not by position.

## Session behaviour

- **Cancel on disconnect.** When a connection drops, its resting orders are
  cancelled. Disable this with `--no-cancel-on-disconnect`.
- **Self-trade prevention.** Each connection is its own account, so orders from
  the same connection never trade with each other. The incoming order's
  remainder is cancelled with `SELF_TRADE`.
- **Rate limit.** By default each session may send 100,000 messages per second
  (`--max-rate`). Messages over the limit get `REJECT … THROTTLED` or
  `ERROR throttled`.
- **Slow consumers.** If more than 8 MiB of replies are queued for a client that
  isn't reading, the connection is closed. This stops one slow reader from
  growing the server's memory without bound.
- **Session limit.** Connections beyond `--max-sessions` receive
  `ERROR server full` and are closed.
