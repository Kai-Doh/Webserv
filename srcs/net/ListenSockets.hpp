#ifndef LISTENSOCKETS_HPP
# define LISTENSOCKETS_HPP

# include <vector>

# include "net/ListenAddr.hpp"
# include "net/Socket.hpp"

std::vector<Socket*>	createListenSockets(const std::vector<ListenAddr>& addrs);

void					destroyListenSockets(std::vector<Socket*>& sockets);

#endif
