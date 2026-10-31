#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

#include "itch.hpp"

namespace mm::itch
{

// helper functions to read 2, 4 and 8 bytes from our packet stream
// we need to memcpy (to read) and byteswap (for little endianness).

namespace
{
// here we need functions to read each type of order, starting from a byte, then return the type.
// addorder
AddOrderFields decode_add_order(const std::byte* packet)
{
    // need to read 5 params, and we are given the address of the MESSAGE start.
    std::uint64_t order_ref = read_be64(packet, AddOrder::order_ref);
    mm::Side      side;
    auto          temp_side = static_cast<char>(packet[AddOrder::side]);
    switch (temp_side)
    {
        // here we are checking, the byte that we read, does it represent "66" or "83" ? (B/S)
        case 'B':
            side = mm::Side::Buy;
            break;
        case 'S':
            side = mm::Side::Sell;
            break;
        default:
            // TODO followup on this
            break;
    }
    std::uint32_t shares = read_be32(packet, AddOrder::shares);
    std::uint32_t price  = read_be32(packet, AddOrder::price);
    // whats the best way to return these?
    AddOrderFields decoded_add_order{order_ref, side, shares, price};
    return decoded_add_order;
}

OrderExecutedFields decode_executed_order(const std::byte* packet)
{
    // need to read two params
    std::uint64_t       order_ref       = read_be64(packet, OrderExecuted::order_ref);
    std::uint32_t       executed_shares = read_be32(packet, OrderExecuted::executed_shares);
    OrderExecutedFields executed_order{order_ref, executed_shares};
    return executed_order;
}

OrderCancelFields decode_cancel_order(const std::byte* packet)
{
    // partial cancel. cancelled_shares is how much to remove from the level.
    std::uint64_t     order_ref        = read_be64(packet, OrderCancel::order_ref);
    std::uint32_t     cancelled_shares = read_be32(packet, OrderCancel::cancelled_shares);
    OrderCancelFields cancel_order{order_ref, cancelled_shares};
    return cancel_order;
}

OrderDeleteFields decode_delete_order(const std::byte* packet)
{
    // only one field on the wire. the quantity to remove is the order's remaining
    // shares, which only the order map knows.
    std::uint64_t     order_ref = read_be64(packet, OrderDelete::order_ref);
    OrderDeleteFields delete_order{order_ref};
    return delete_order;
}

OrderReplaceFields decode_replace_order(const std::byte* packet)
{
    // two order refs. delete-then-add: remove original_order_ref's full remaining from
    // its old level, erase it, then insert new_order_ref at the new price/shares.
    // every later message uses the NEW ref.
    std::uint64_t      original_order_ref = read_be64(packet, OrderReplace::original_order_ref);
    std::uint64_t      new_order_ref      = read_be64(packet, OrderReplace::new_order_ref);
    std::uint32_t      shares             = read_be32(packet, OrderReplace::shares);
    std::uint32_t      price              = read_be32(packet, OrderReplace::price);
    OrderReplaceFields replace_order{original_order_ref, new_order_ref, shares, price};
    return replace_order;
}

} // namespace

// "span" is a view over a contigous seuqence of objects it holds a pointer to the start of some
// data, and a size. in our case, this is the start of the buffer, and the len of the packet.

// we know that for our incoming data, the message header will have:

// 0   msg type        1 (char)
// 1   stock locate    2 byte integer
// 3   tracking num    2 byte integer
// 5   timestamp       6 byte nanoseconds since midnight.

void decode_packet(std::span<const std::byte> packet)
{

    // general flow:
    // 1) parse message header. bail if it's > the end
    // 2) get message type. based on that, decode the message!
    // 3) use jump table to jump to the correct parsing logic
    // 4) parse it into variables using memcpy + bswap
    // now it's in the variable, as little endianness
    // 5) call our market state function with the information
    // 6) use std::span's subspan feature to shrink the size of
    // the area we are considering (subspan(start_packet, range))
    // i.e packet = packet.subspan(offset of next starting message, new size)
    // subspan also reduces the size for us!

    std::size_t offset = 0;

    // / TODO: decide if the decode_type funtions are being inlined, otherwise
    // force them to be inlined. NOTE: inlining is not optional here -- AddOrderFields is
    // 24 bytes, over the 16-byte register-return limit on AAPCS64, and no member ordering
    // gets it under (8+4+4+1 = 17 minimum). So if it is ever out-of-lined we pay a hidden
    // sret stack round-trip per message. Verify with -Rpass-missed=inline.
    // TODO: the length check and `offset +=` are duplicated in all 7 cases. Once
    // length_for(type) exists over all ~20 ITCH types, hoist both above the switch:
    // one bounds check, one advance, no case can forget, and a length of 0 becomes the
    // single "unknown type, cannot frame" bail -- which also fixes the default: hang.
    // TODO: put [[unlikely]] on the truncation branches and [[gnu::cold]] on the error
    // reporter, so the never-taken paths stop occupying I-cache next to the decoders.

    while (offset < packet.size())
    {
        // PARSE HEADER

        // TODO add and parse timestamp here.

        // read message type (dont need memcpy, can directly read!)
        auto type = static_cast<itch::MsgType>(packet.data()[offset + MessageHeader::msg_type]);
        // TODO: stock_locate is decoded and then DISCARDED. It is the only symbol id in
        // the message, so until it reaches the book every symbol collapses into one book
        // (AAPL and MSFT adds applied to the same state). The locate -> book index
        // dispatch is a per-message lookup, so design it for cache locality before
        // state/ exists.
        std::uint16_t stock_locate = read_be16(packet.data(), offset + MessageHeader::stock_locate);
        // we can disregard tracking num & timestamp, it doesnt matter.
        // jump table with case : type, then parse the rest of the type.
        switch (type)
        {
            case MsgType::AddOrder:
            {
                // does this whole message fit in what's left? MUST come before
                // decode_add_order touches any bytes. written as `remaining < length`
                // rather than `offset + length > size` so it cannot overflow.
                // the subtraction is safe: the loop condition guarantees offset < size.
                if (AddOrder::length > packet.size() - offset)
                {
                    return; // truncated message. TODO: count it, cold path
                }

                AddOrderFields addorderfields = decode_add_order(packet.data() + offset);
                // logic to submit into market making book

                offset += AddOrder::length; // advance to the next message
                break;
            };

            case MsgType::AddOrderWithMPID:
            {
                if (AddOrderWithMPID::length > packet.size() - offset)
                {
                    // TODO: error handle
                    return;
                }
                // (F) is (A) + a 4-byte MPID appended, so all of AddOrder's offsets apply.
                // ONLY the length differs -- 40 vs 36. this IS an add: skipping it would
                // silently lose orders, and every later E/X/D for those refs would miss.
                AddOrderFields addorderfields = decode_add_order(packet.data() + offset);
                // logic to submit into market making book

                offset += AddOrderWithMPID::length;
                break;
            }
            case MsgType::OrderExecuted:
            {
                if (OrderExecuted::length > packet.size() - offset)
                {
                    // TODO: error handle
                    return;
                }
                OrderExecutedFields executed_order = decode_executed_order(packet.data() + offset);
                // TODO: market making book
                offset += OrderExecuted::length;
                break;
            }

            case MsgType::OrderExecutedWithPrice:
            {
                if (OrderExecutedWithPrice::length > packet.size() - offset)
                {
                    // TODO: error handle
                    return;
                }
                // same decoder as (E): order_ref and executed_shares sit at the same offsets,
                // and printable / execution_price are tape concerns we don't read.
                // ONLY the length differs -- 36 vs 31. reuse the decoder, never the length.
                OrderExecutedFields executed_order = decode_executed_order(packet.data() + offset);
                // TODO: market making book
                offset += OrderExecutedWithPrice::length;
                break;
            }

            case MsgType::OrderCancel:
            {
                if (OrderCancel::length > packet.size() - offset)
                {
                    // TODO: error handle
                    return;
                }
                OrderCancelFields cancel_order = decode_cancel_order(packet.data() + offset);
                // TODO: market making book
                offset += OrderCancel::length;
                break;
            }

            case MsgType::OrderDelete:
            {
                if (OrderDelete::length > packet.size() - offset)
                {
                    // TODO: error handle
                    return;
                }
                OrderDeleteFields delete_order = decode_delete_order(packet.data() + offset);
                // TODO: market making book -- remove ref.remaining from its level, erase ref
                offset += OrderDelete::length;
                break;
            }

            case MsgType::OrderReplace:
            {
                if (OrderReplace::length > packet.size() - offset)
                {
                    // TODO: error handle
                    return;
                }
                OrderReplaceFields replace_order = decode_replace_order(packet.data() + offset);
                // TODO: market making book -- this is a DELETE + ADD, not a modify.
                // remove original_order_ref's full remaining from its old level, erase it,
                // then insert new_order_ref at the new price. all later messages use the new ref.
                offset += OrderReplace::length;
                break;
            }

            default:
                // TODO: write error path, make cold path
                break;
        }
    }
}
}; // namespace mm::itch