#include "core/Server.hpp"

#include <ctime>
#include <set>
#include <stdexcept>
#include <unistd.h>

/**
 * @brief Construit une struct pollfd (une struct C : pas de constructeur, donc on la remplit)
 * @param fd Descripteur de fichier a surveiller
 * @param events Evenements voulus (POLLIN, POLLOUT, ...)
 * @return La struct initialisee (revents vaut 0)
 */
static struct pollfd	make_pollfd(int fd, short events)
{
	struct pollfd	p;

	p.fd = fd;
	p.events = events;
	p.revents = 0;
	return (p);
}

/**
 * @brief Ajoute un fd a la liste donnee a poll()
 *        ATTENTION : peut reallouer le vecteur, donc tout pointeur /
 *        reference / iterateur vers _poll_fds devient invalide ensuite (utiliser des index).
 * @param fd Descripteur de fichier a surveiller
 * @param events Evenements voulus (POLLIN, POLLOUT)
 */
void	Server::addPollFd(int fd, short events)
{
	_poll_fds.push_back(make_pollfd(fd, events));
}

/**
 * @brief Retire un fd de la liste donnee a poll() (ne fait rien s'il est absent)
 * @param fd Descripteur de fichier a ne plus surveiller
 */
void	Server::removePollFd(int fd)
{
	for (size_t i = 0; i < _poll_fds.size(); ++i)
	{
		if (_poll_fds[i].fd == fd)
		{
			_poll_fds.erase(_poll_fds.begin() + i);
			return ;
		}
	}
}

/**
 * @brief Change ce que poll() doit surveiller sur un fd (ex. POLLIN -> POLLOUT)
 * @param fd Descripteur de fichier deja enregistre dans poll_fds
 * @param events Nouveau masque d'evenements
 * @return true si le fd a ete trouve et mis a jour, false sinon
 */
bool	Server::setPollEvents(int fd, short events)
{
	for (size_t i = 0; i < _poll_fds.size(); ++i)
	{
		if (_poll_fds[i].fd == fd)
		{
			_poll_fds[i].events = events;
			return (true);
		}
	}
	return (false);
}

/**
 * @brief Indique si un fd est l'un de nos sockets d'ecoute
 * @param fd Descripteur de fichier a tester
 * @return true pour un fd d'ecoute, false pour un fd client (ou inconnu)
 */
bool	Server::isListenFd(int fd) const
{
	return (_listen_fds.find(fd) != _listen_fds.end());
}

/**
 * @brief Enregistre un client tout juste accepte.
 * @param client_fd fd renvoye par accept() (deja non-bloquant)
 * @return Pointeur vers la Connection stockee (valide jusqu'a ce que ce fd soit retire)
 * @throw std::logic_error si le fd est invalide ou deja enregistre
 */
Connection	*Server::addConnection(int client_fd)
{
	std::pair<ConnMap::iterator, bool>	res;
	Connection							conn;

	if (client_fd < 0 || isListenFd(client_fd))
		throw std::logic_error("addConnection: invalid fd");
	conn.fd = client_fd;
	conn.state = READING_REQUEST;
	conn.last_activity = time(NULL);
	res = _connections.insert(std::make_pair(client_fd, conn));
	if (!res.second)
		throw std::logic_error("addConnection: fd already registered");
	try
	{
		addPollFd(client_fd, POLLIN);
	}
	catch (...)
	{
		_connections.erase(res.first);
		throw;
	}
	return (&res.first->second);
}

/**
 * @brief Recherche la Connection d'un fd client
 * @param fd Descripteur de fichier client
 * @return Pointeur vers sa Connection, ou NULL si ce fd n'est pas un client
 */
Connection	*Server::findConnection(int fd)
{
	ConnMap::iterator	it = _connections.find(fd);

	if (it == _connections.end())
		return (NULL);
	return (&it->second);
}

/**
 * @brief Prepare une Connection a recevoir une autre requete sur le meme fd
 * @param conn Connection dont la reponse a ete entierement envoyee et qu'on
 *             garde ouverte pour une autre requete
 */
void	Server::resetConnectionForReuse(Connection& conn)
{
	conn.state = READING_REQUEST;
	conn.write_buffer.clear();
	conn.bytes_written = 0;
	conn.method.clear();
	conn.path.clear();
	conn.http_version.clear();
	conn.headers.clear();
	conn.body.clear();
	conn.query_string.clear();
	conn.cgi_path_info.clear();
	conn.status_code = 0;
	conn.cgi_stdin_fd = -1;
	conn.cgi_stdout_fd = -1;
	conn.cgi_pid = -1;
	conn.cgi_out.clear();
	conn.cgi_in_offset = 0;
	conn.headers_ready = false;
	conn.body_start = 0;
	conn.chunked_scan_pos = 0;
}

/**
 * @brief Oublie completement un client : CGI en cours (s'il y en a un),
 *        entree poll_fds, Connection, puis close(fd)
 *        Un client peut partir (timeout, erreur socket) pendant qu'un CGI
 *        tourne encore pour lui (etat CGI_RUNNING) : sans ce nettoyage, ses
 *        pipes resteraient enregistres dans poll_fds pour un fd deja ferme,
 *        et le processus CGI ne serait jamais tue ni reapable.
 * @param fd Descripteur de fichier client
 */
void	Server::removeConnection(int fd)
{
	ConnMap::iterator	it = _connections.find(fd);

	if (it == _connections.end())
		return ;
	killCgi(it->second);
	removePollFd(fd);
	_connections.erase(it);
	close(fd);
}

/**
 * @brief Verifie que poll_fds et les deux maps sont coherents entre eux
 * @return true si coherent, false si les structures ont diverge
 */
bool	Server::checkInvariant(void) const
{
	std::set<int>	seen;

	if (_poll_fds.size() != _listen_fds.size() + _connections.size() + _cgi_owner.size())
		return (false);
	for (size_t i = 0; i < _poll_fds.size(); ++i)
	{
		int	fd = _poll_fds[i].fd;

		if (!seen.insert(fd).second)
			return (false);
		if (!isListenFd(fd) && _connections.find(fd) == _connections.end()
			&& _cgi_owner.find(fd) == _cgi_owner.end())
			return (false);
	}
	for (ConnMap::const_iterator it = _connections.begin(); it != _connections.end(); ++it)
	{
		if (it->second.fd != it->first || isListenFd(it->first))
			return (false);
	}
	return (true);
}
