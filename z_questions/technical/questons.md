Why do normal hasmaps (unordered_map) cause CPU cache misses? what is the way/design around them in HFT?

In SBE, what is the templateId? who defines it? who passes it? whose responsibility is it?

What is good struct hygiene? What order of primitives do we need?

What do we need to do end to end, to define an SBE layout, and accept information?

How do we outline offsets for binary formats? Why are structs fine? How do I know what slows down performance? i.e I dont know if a struct with the variables is slower or faster
than just having a bunch of variables individually.

What is memcpy + bytesawap?

What does "on the wire" mean?

How do we write offset? if something occupies bytes 0,1,2,3 then is the offset for the next type "4" ? i.e it starts at byte = 4?

Why does the size/order of the content we recieve not actually matter? Does it matter for our buffer?

Why is CPP like this?

```
alignof(struct) = the largest alignof among its members
sizeof(struct)  = rounded up to a multiple of alignof(struct)
each member     = placed at an offset that's a multiple of its own alignment
```
