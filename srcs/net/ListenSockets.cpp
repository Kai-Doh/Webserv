#include "net/ListenSockets.hpp"

#include <cstddef>

/**
 * @brief Indique si une paire interface:port a deja un socket d'ecoute
 * @param sockets Sockets d'ecoute deja crees
 * @param addr Paire recherchee
 * @return true si un host + port identique est deja ecoute
 */
static bool	is_already_listening(const std::vector<Socket*>& sockets, const ListenAddr& addr)
{
	for (size_t i = 0; i < sockets.size(); ++i)
	{
		if (sockets[i]->getHost() == addr.host
			&& sockets[i]->getPort() == addr.port)
			return (true);
	}
	return (false);
}

/**
 * @brief Cree un socket d'ecoute par paire interface:port distincte
 *        Les paires identiques sont ignorees (deux blocs "server" peuvent
 *        partager un port). Tout ou rien : si un socket echoue, ceux deja
 *        ouverts sont fermes.
 * @param addrs Paires a ecouter (depuis le fichier de configuration)
 * @return Les sockets d'ecoute, prets a etre donnes a poll()
 * @throw std::runtime_error avec l'appel et l'adresse en cause
 */
std::vector<Socket*>	createListenSockets(const std::vector<ListenAddr>& addrs)
{
	std::vector<Socket*>	sockets;
	Socket					*sock;

	sockets.reserve(addrs.size());	// push_back ne pourra plus lever d'exception
	try
	{
		for (size_t i = 0; i < addrs.size(); ++i)
		{
			if (is_already_listening(sockets, addrs[i]))
				continue;
			sock = new Socket(addrs[i].host, addrs[i].port);
			sockets.push_back(sock);
			sock->setup();
		}
	}
	catch (...)
	{
		destroyListenSockets(sockets);
		throw;
	}
	return (sockets);
}

/**
 * @brief Ferme et detruit chaque socket d'ecoute, puis vide le vecteur
 * @param sockets Sockets renvoyes par createListenSockets
 */
void	destroyListenSockets(std::vector<Socket*>& sockets)
{
	for (size_t i = 0; i < sockets.size(); ++i)
		delete sockets[i];
	sockets.clear();
}
