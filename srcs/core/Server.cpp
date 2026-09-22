#include "core/Server.hpp"

#include <stdexcept>
#include <unistd.h>

#include "net/ListenSockets.hpp"

/**
 * @brief Transforme la directive "listen" de chaque ServerConfig en ListenAddr
 * @param configs Blocs de configuration lus par Config::load (src/config)
 * @return Un ListenAddr par config, dans le meme ordre (createListenSockets
 *         retire les doublons : deux "server{}" peuvent partager un host:port)
 */
static std::vector<ListenAddr>	listen_addrs_from_configs(const std::vector<ServerConfig>& configs)
{
	std::vector<ListenAddr>	addrs;

	addrs.reserve(configs.size());
	for (size_t i = 0; i < configs.size(); ++i)
		addrs.push_back(ListenAddr(configs[i].host, configs[i].port));
	return (addrs);
}

/**
 * @brief Ouvre les sockets d'ecoute et les enregistre dans poll_fds (POLLIN)
 * @param configs Blocs "server{}" parses (au moins un), voir src/config
 * @throw std::runtime_error si un socket ne peut pas etre ouvert ou si configs est vide
 */
Server::Server(const std::vector<ServerConfig>& configs) : _configs(configs), _spare_fd(-1)
{
	_listeners = createListenSockets(listen_addrs_from_configs(_configs));
	try
	{
		if (_listeners.empty())
			throw std::runtime_error("no listening socket to open");
		for (size_t i = 0; i < _listeners.size(); ++i)
		{
			int	fd = _listeners[i]->getFd();

			_listen_fds[fd] = _listeners[i];
			addPollFd(fd, POLLIN);
			for (size_t c = 0; c < _configs.size(); ++c)
			{
				if (_configs[c].host == _listeners[i]->getHost()
					&& _configs[c].port == _listeners[i]->getPort())
				{
					_listener_config[fd] = &_configs[c];
					break ;
				}
			}
		}
		if (!reserveSpareFd())
			throw std::runtime_error("cannot reserve a spare file descriptor");
	}
	catch (...)
	{
		if (_spare_fd != -1)
			close(_spare_fd);
		destroyListenSockets(_listeners);
		throw;
	}
}

/**
 * @brief Ferme tous les fd clients restants, le fd de reserve, puis les sockets d'ecoute
 */
Server::~Server(void)
{
	for (ConnMap::iterator it = _connections.begin(); it != _connections.end(); ++it)
		close(it->first);
	_connections.clear();
	if (_spare_fd != -1)
		close(_spare_fd);
	_poll_fds.clear();
	_listen_fds.clear();
	destroyListenSockets(_listeners);
}
