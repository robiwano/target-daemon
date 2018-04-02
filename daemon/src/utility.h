#pragma once

#include <asio.hpp>
#include <vector>

namespace utility
{
    std::vector<asio::ip::address> get_interface_addresses();
}
