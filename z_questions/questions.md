# Open questions

Delete a question once a note answers it.

## Technical

What is good struct hygiene? What order of primitives do we need?

What do we need to do end to end, to define an SBE layout, and accept information?

How do we outline offsets for binary formats? Why are structs fine? How do I know what slows down performance? i.e I dont know if a struct with the variables is slower or faster
than just having a bunch of variables individually.

What does "on the wire" mean?

Why does the size/order of the content we recieve not actually matter? Does it matter for our buffer?

Order map: on a replayed real feed day, what's the peak live-order count (→ CAPACITY), and which wins on p99: our one shared map, per-symbol maps, or a direct array orders[ref]?

## Finance

What is a market data book? How is it stored/updated locally?

How do markets send data to firms as fast as possible? What is all the types of data that firms even send out to markets? Just orders/cancels?
