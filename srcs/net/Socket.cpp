#include "net/Socket.hpp"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "net/FdUtils.hpp"

/**
 * @brief Construit l'exception levee quand un appel reseau echoue
 * @param what Nom de l'appel en echec (ex. "bind")
 * @param addr Adresse lisible ("host:port")
 * @param reason Message systeme (strerror / gai_strerror)
 * @return Une exception du type : "bind(0.0.0.0:8080): Address already in use"
 */
static std::runtime_error	make_error(const std::string& what, const std::string& addr, const char *reason)
{
	return (std::runtime_error(what + "(" + addr + "): " + reason));
}

/**
 * @brief Stocke l'adresse a ecouter (aucun appel systeme pour l'instant)
 * @param host Adresse IPv4 / nom, ou vide / "*" pour toutes les interfaces
 * @param port Port TCP, entre 1 et 65535
 * @throw std::invalid_argument si le port est hors limites
 */
Socket::Socket(const std::string& host, int port)
	: _host(host), _port(port), _fd(-1)
{
	if (port < 1 || port > 65535)
		throw std::invalid_argument("invalid port (must be 1-65535)");
}

/**
 * @brief Ferme le descripteur de fichier s'il est encore ouvert
 */
Socket::~Socket(void)
{
	closeSocket();
}

/**
 * @brief Construit "host:port" pour les messages d'erreur ("*" si host est vide)
 * @return L'etiquette
 */
std::string	Socket::addressLabel(void) const
{
	std::ostringstream	oss;

	oss << (_host.empty() ? "*" : _host) << ":" << _port;
	return (oss.str());
}

/**
 * @brief Cree le socket TCP (IPv4, stream)
 * @throw std::runtime_error si socket() echoue (ex. trop de fichiers ouverts)
 */
void	Socket::createSocket(void)
{
	int	saved_errno;

	_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (_fd < 0)
	{
		saved_errno = errno;
		throw make_error("socket", addressLabel(), strerror(saved_errno));
	}
}

/**
 * @brief Active SO_REUSEADDR pour que le port puisse etre reutilise juste apres un redemarrage
 * @throw std::runtime_error si setsockopt() echoue
 */
void	Socket::setReuseAddr(void)
{
	int	opt = 1;
	int	saved_errno;

	if (setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
	{
		saved_errno = errno;
		throw make_error("setsockopt(SO_REUSEADDR)", addressLabel(),
			strerror(saved_errno));
	}
}

/**
 * @brief Passe le socket en mode non-bloquant (voir setFdNonBlocking)
 * @throw std::runtime_error si fcntl() echoue
 */
void	Socket::setNonBlocking(void)
{
	int	saved_errno;

	if (!setFdNonBlocking(_fd))
	{
		saved_errno = errno;
		throw make_error("fcntl(O_NONBLOCK)", addressLabel(),
			strerror(saved_errno));
	}
}

/**
 * @brief Resout host:port avec getaddrinfo() et y lie le socket
 * @throw std::runtime_error si l'hote ne peut pas etre resolu ou si bind() echoue
 *        (typiquement "Address already in use" ou "Permission denied")
 */
void	Socket::bindAddress(void)
{
	struct addrinfo		hints;
	struct addrinfo		*res = NULL;
	std::ostringstream	port_oss;
	std::string			port_str;
	const char			*node;
	int					ret;
	int					saved_errno;

	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	port_oss << _port;
	port_str = port_oss.str();
	node = (_host.empty() || _host == "*") ? NULL : _host.c_str();
	ret = getaddrinfo(node, port_str.c_str(), &hints, &res);
	if (ret != 0)
		throw make_error("getaddrinfo", addressLabel(), gai_strerror(ret));
	if (bind(_fd, res->ai_addr, res->ai_addrlen) < 0)
	{
		saved_errno = errno;
		freeaddrinfo(res);
		throw make_error("bind", addressLabel(), strerror(saved_errno));
	}
	freeaddrinfo(res);
}

/**
 * @brief Marque le socket comme passif : le noyau met desormais les clients entrants en file
 * @throw std::runtime_error si listen() echoue
 */
void	Socket::startListening(void)
{
	int	saved_errno;

	if (listen(_fd, SOMAXCONN) < 0)
	{
		saved_errno = errno;
		throw make_error("listen", addressLabel(), strerror(saved_errno));
	}
}

/**
 * @brief Execute chaque etape d'installation dans l'ordre ; laisse le socket ferme en cas d'echec
 *        socket -> SO_REUSEADDR -> O_NONBLOCK -> bind -> listen
 * @throw std::runtime_error (depuis une etape) ou std::logic_error si deja configure
 */
void	Socket::setup(void)
{
	if (_fd != -1)
		throw std::logic_error("Socket::setup called twice on " + addressLabel());
	try
	{
		createSocket();
		setReuseAddr();
		setNonBlocking();
		bindAddress();
		startListening();
	}
	catch (...)
	{
		closeSocket();
		throw;
	}
}

/**
 * @brief Ferme le descripteur de fichier (peut etre appelee plusieurs fois sans risque)
 */
void	Socket::closeSocket(void)
{
	if (_fd != -1)
	{
		close(_fd);
		_fd = -1;
	}
}

/**
 * @brief Donne le descripteur de fichier a enregistrer dans poll()
 * @return Le fd, ou -1 si le socket n'est pas ouvert
 */
int	Socket::getFd(void) const
{
	return (_fd);
}

/**
 * @brief Donne l'interface sur laquelle ce socket ecoute
 * @return L'hote tel que donne a la construction (vide = toutes les interfaces)
 */
const std::string&	Socket::getHost(void) const
{
	return (_host);
}

/**
 * @brief Donne le port TCP sur lequel ce socket ecoute
 * @return Le numero de port
 */
int	Socket::getPort(void) const
{
	return (_port);
}
