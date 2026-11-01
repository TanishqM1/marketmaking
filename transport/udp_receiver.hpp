#pragma once

#include <cstddef> // std::byte (raw bytes), std::size_t (BUFFER_SIZE)
#include <cstdint> // std::uint16_t (port number)
#include <span>    // std::span -- receive()'s return type, {ptr, size}

namespace