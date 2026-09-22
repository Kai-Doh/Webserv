#ifndef SERVER_HPP
# define SERVER_HPP

# include <map>
# include <ostream>
# include <vector>
# include <poll.h>

# include "net/ListenAddr.hpp"
# include "net/Socket.hpp"
# include "shared/connection.hpp"
# include "Config.hpp"

class Server
{
public:
	Server(const std::vector<ServerConfig>& configs);
	~Server(void);

	void	run(void);					// ServerLoop.cpp
	void	dump(std::ostream& os) const;	// ServerDebug.cpp
	bool	checkInvariant(void) const;		// ServerRegistry.cpp

private:
	Server(const Server& other);
	Server&	operator=(const Server& other);

	typedef std::map<int, Connection>		ConnMap;
	typedef std::map<int, Socket*>			ListenMap;
	typedef std::map<int, const ServerConfig*>	ListenConfigMap;

	/* --- poll_fds : la liste donnee a poll()        (ServerRegistry.cpp) --- */
	void			addPollFd(int fd, short events);
	void			removePollFd(int fd);
	bool			setPollEvents(int fd, short events);

	/* --- registre : poll_fds + connections synchronises (ServerRegistry.cpp) --- */
	bool			isListenFd(int fd) const;
	Connection*		addConnection(int client_fd);
	Connection*		findConnection(int fd);
	void			resetConnectionForReuse(Connection& conn);
	void			removeConnection(int fd);

	/* --- boucle principale (partie 3)                (ServerLoop.cpp) --- */
	void			installSignalHandlers(void);
	void			runOnce(int timeout_ms);
	void			dispatchEvents(void);
	void			handleClientEvent(Connection& conn, short revents);

	/* --- nouvelles connexions (partie 4)             (ServerAccept.cpp) --- */
	bool			reserveSpareFd(void);
	void			acceptNewClient(int listen_fd);
	void			rejectWhenOutOfFds(int listen_fd);

	/* --- lecture des requetes (partie 5)                 (ServerRead.cpp) --- */
	void			handleRead(Connection& conn);

	/* --- transition vers le traitement (partie 6)    (ServerHandlers.cpp) --- */
	void			handleProcessing(Connection& conn);

	/* --- envoi des reponses (partie 7)                    (ServerWrite.cpp) --- */
	void			handleWrite(Connection& conn);

	/* --- timeouts (partie 9)                            (ServerTimeouts.cpp) --- */
	void			sweepTimeouts(void);

	std::vector<ServerConfig>	_configs;		// blocs "server{}" parses (src/config)
	std::vector<Socket*>		_listeners;		// possede les sockets d'ecoute
	ListenMap					_listen_fds;	// fd d'ecoute -> son Socket (non possede)
	ListenConfigMap				_listener_config;	// fd d'ecoute -> la ServerConfig qu'il sert
	std::vector<struct pollfd>	_poll_fds;		// le tableau donne a poll()
	ConnMap						_connections;	// fd client -> sa Connection
	int							_spare_fd;		// fd garde en reserve (/dev/null), voir rejectWhenOutOfFds
};

#endif
