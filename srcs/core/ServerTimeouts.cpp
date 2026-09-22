#include "core/Server.hpp"

#include <ctime>
#include <iostream>

namespace {
const time_t	CONNECTION_TIMEOUT_SECONDS = 60;
}

/**
 * @brief Ferme de force toute connexion restee silencieuse trop longtemps
 */
void	Server::sweepTimeouts(void)
{
	std::vector<int>			expired;
	time_t						now;
	ConnMap::const_iterator		it;

	now = time(NULL);
	for (it = _connections.begin(); it != _connections.end(); ++it)
	{
		const Connection&	conn = it->second;

		if (conn.state == CGI_RUNNING)
			continue ;
		if (now - conn.last_activity >= CONNECTION_TIMEOUT_SECONDS)
			expired.push_back(conn.fd);
	}
	for (size_t i = 0; i < expired.size(); ++i)
	{
		std::cerr << "[timeout] fd " << expired[i] << " : inactif depuis "
			<< CONNECTION_TIMEOUT_SECONDS << "s ou plus -> fermeture forcee"
			<< std::endl;
		removeConnection(expired[i]);
	}
}
