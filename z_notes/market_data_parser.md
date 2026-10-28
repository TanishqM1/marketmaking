Phase 1) Parsing market data as per SBE, and construct the minimal things we need from it. 

1. Define XML Schema `codec/schema`
2. Define primitive typs structs `types/` for type safety
3. Create Encocder + Decoder for one message type (Add Order) to get it in memory. `codec/`
4. Write tests to check encode -> decode -> compare path. `tests/codec`
5. Repeat for all message types.

This is the format that we will recieve data in (but SBE of course)
```
Add Order. If the type is "F", there will *also* be a 4-byte MPID. We do not need to worry about this, and will assume we always parse it as an "A". (regardless, we can check the len())
{
  "message_type": "A",
  "timestamp": 34200000000000, 
  "order_reference_number": 1029485761,
  "buy_sell_indicator": "B",
  "shares": 500,
  "stock": "AAPL",
  "price": 175.2500
}

Order Executed. if the type is "C", that means it occured at a bette price than the original order limit.
{
  "message_type": "E",
  "timestamp": 34200105000000,
  "order_reference_number": 1029485761,
  "executed_shares": 200,
  "match_number": 58294
}

Order Cancel (X), Order Modify (U), Order Delete (D)
{
  "message_type": "X",
  "timestamp": 34200250000000,
  "order_reference_number": 1029485761,
  "canceled_shares": 300
}
```


Market Data books do not need to:
    pair aggressors with resting orders, or generate fills (data to notify the orderee)
    enforce or track time priority / queue position
    maintain per-order queues with price levels
    care about MPID / optional market maker identifiers

The market does a great job of helping us hadle our local data state.
    # of shares at each price (bids descending)
    # of shares at each price (asks aschending)
Now when trades are matched, we get "E" orders to know what order we need to delete (the non-aggresor), and the remaining qty from 
the aggressor (if there is one!) comes in as a separate "A" add order

Parsing SBE:
1) We read message header (first 8 bytes of the payload)
2) Then we utilize a jump table, to pick the right SBE template parser.
3) we use SBE parsing based on the template for the unique use case, and now we have the [variables] or data ready to use.

Generally the data we recieve from exchanges is Big Endian.