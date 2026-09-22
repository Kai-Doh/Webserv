#include "core/Server.hpp"

#include <cerrno>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net/FdUtils.hpp"

// Nombre maximal de clients acceptes d'un coup pour UN reveil de poll() 
static const int	ACCEPT_BATCH = 64;

/**
 * @brief Decrit un socket d'ecoute pour les logs
 * @param sock Le socket d'ecoute
 * @return "host:port" ("*" s'il ecoute sur toutes les interfaces)
 */
static std::string	describe_listener(const Socket& sock)
{
	std::ostringstream	oss;

	oss << (sock.getHost().empty() ? "*" : sock.getHost()) << ":" << sock.getPort();
	return (oss.str());
}

// Nouvelles connexions

/**
 * @brief Ouvre (si ce n'est pas deja fait) le descripteur de fichier de "reserve"
 *        C'est la sortie de secours quand le processus n'a plus de descripteurs :
 *        voir rejectWhenOutOfFds()
 * @return true si le fd de reserve est disponible, false si open() a echoue
 */
bool	Server::reserveSpareFd(void)
{
	if (_spare_fd != -1)
		return (true);
	_spare_fd = open("/dev/null", O_RDONLY);
	return (_spare_fd != -1);
}

/**
 * @brief Accepte les clients en attente sur un socket d'ecoute que poll() a
 *        signale pret, et enregistre chacun d'eux (non-bloquant, Connection,
 *        poll_fds). Prend au maximum ACCEPT_BATCH clients par appel, s'il en
 *        reste, poll() signalera de nouveau le socket d'ecoute au tour suivant.
 * @param listen_fd Socket d'ecoute que poll() a signale pret (POLLIN)
 */
void	Server::acceptNewClient(int listen_fd)
{
	ListenMap::const_iterator			listener = _listen_fds.find(listen_fd);
	ListenConfigMap::const_iterator	conf_it = _listener_config.find(listen_fd);
	const ServerConfig					*conf = (conf_it != _listener_config.end()) ? conf_it->second : NULL;
	Connection							*conn;
	int									client_fd;
	int									saved_errno;

	for (int n = 0; n < ACCEPT_BATCH; ++n)
	{
		client_fd = accept(listen_fd, NULL, NULL);
		if (client_fd < 0)
		{
			saved_errno = errno;
			if (saved_errno == EINTR || saved_errno == ECONNABORTED
				|| saved_errno == EPROTO)
				continue ;		// erreur passagere : un autre client attend peut-etre
			if (saved_errno == EMFILE || saved_errno == ENFILE)
				rejectWhenOutOfFds(listen_fd);
			else if (saved_errno != EAGAIN)		// EAGAIN (== EWOULDBLOCK) : file vide, et c'est Okay
				std::cerr << "accept: " << strerror(saved_errno) << std::endl;
			return ;
		}
		if (!setFdNonBlocking(client_fd))
		{
			close(client_fd);
			continue ;
		}
		try
		{
			conn = addConnection(client_fd);
		}
		catch (const std::exception& e)
		{
			std::cerr << "accept: " << e.what() << std::endl;
			close(client_fd);
			return ;
		}
		conn->server_conf = conf;	// utilise par handle_request() (partie 6)
		std::cout << "[accept] client fd " << client_fd << " sur l'ecoute "
			<< (listener != _listen_fds.end() ? describe_listener(*listener->second) : "?")
			<< " (" << _connections.size() << " connexion(s))" << std::endl;
	}
}

/**
 * @brief Se debarrasse d'UN client en attente quand accept() a echoue par
 *        manque de fd
 *        Sans cela, le client resterait dans la file du noyau, poll()
 *        continuerait de signaler le socket d'ecoute comme pret, accept()
 *        continuerait d'echouer.
 *        On libere le fd de reserve, on l'utilise pour accepter ce client, on
 *        le ferme aussitot, puis on reserve de nouveau le fd de secours.
 * @param listen_fd Socket d'ecoute dont l'accept() a echoue
 */
void	Server::rejectWhenOutOfFds(int listen_fd)
{
	int	victim;

	std::cerr << "[accept] plus de descripteurs de fichiers : client refuse" << std::endl;
	if (_spare_fd == -1)
		return ;
	close(_spare_fd);
	_spare_fd = -1;
	victim = accept(listen_fd, NULL, NULL);
	if (victim >= 0)
		close(victim);
	reserveSpareFd();
}
