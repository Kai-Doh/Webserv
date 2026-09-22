#include "core/Server.hpp"

#include <csignal>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

#include "cgi/CgiHandler.hpp"

// CGI : greffer les pipes sur le poll() partage

/**
 * @brief Enregistre les pipes d'un CGI tout juste demarre dans poll_fds
 *        Ne fait rien si conn.state != CGI_RUNNING. Coupe aussi les evenements
 *        du fd client pendant que le CGI tourne : POLLHUP/POLLERR restent
 *        signales quoi qu'il arrive, mais il n'y a rien d'autre a y faire
 *        tant que le CGI n'a pas fini (voir handleClientEvent).
 * @param conn Connexion dont handle_request() vient de demarrer un CGI
 */
void	Server::registerCgiFds(Connection& conn)
{
	if (conn.state != CGI_RUNNING)
		return ;
	setPollEvents(conn.fd, 0);
	if (conn.cgi_stdin_fd != -1)
	{
		addPollFd(conn.cgi_stdin_fd, POLLOUT);
		_cgi_owner[conn.cgi_stdin_fd] = conn.fd;
	}
	if (conn.cgi_stdout_fd != -1)
	{
		addPollFd(conn.cgi_stdout_fd, POLLIN);
		_cgi_owner[conn.cgi_stdout_fd] = conn.fd;
	}
}

/**
 * @brief Route un evenement poll() sur un pipe de CGI vers le bon cote
 *        (onStdinWritable / onStdoutReadable)
 *        Ne retire jamais le pipe lui-meme : erase() au milieu de poll_fds
 *        pendant que dispatchEvents() l'indexe encore decalerait tout ce
 *        qui suit et ferait deraper le reste de son parcours (meme raison
 *        que to_close y est deja retire APRES la boucle, pas dedans). Le
 *        pipe dont le proprietaire a deja disparu compte aussi comme "a
 *        retirer" : sweepCgi() reste responsable de tuer/reaper le
 *        processus lui-meme.
 * @param fd      Pipe (stdin ou stdout du CGI) que poll() a signale pret
 * @param revents Evenements signales pour ce fd
 * @return true si l'appelant doit retirer ce pipe de poll_fds (fermeture
 *         cote CGI, ou proprietaire deja disparu), false sinon
 */
bool	Server::handleCgiEvent(int fd, short revents)
{
	CgiPipeMap::iterator	owner_it = _cgi_owner.find(fd);
	Connection				*conn;

	if (owner_it == _cgi_owner.end())
		return (false);
	conn = findConnection(owner_it->second);
	if (conn == NULL)
		return (true);
	if (fd == conn->cgi_stdin_fd && (revents & (POLLOUT | POLLERR | POLLHUP)))
	{
		cgi_handler::onStdinWritable(*conn);
		return (conn->cgi_stdin_fd == -1);
	}
	if (fd == conn->cgi_stdout_fd && (revents & (POLLIN | POLLHUP | POLLERR)))
	{
		cgi_handler::onStdoutReadable(*conn);
		return (conn->cgi_stdout_fd == -1);
	}
	return (false);
}

/**
 * @brief Retire un pipe de CGI de poll_fds, le ferme, et oublie son proprietaire
 * @param fd Pipe (stdin ou stdout d'un CGI) a oublier
 */
void	Server::removeCgiPipe(int fd)
{
	removePollFd(fd);
	close(fd);
	_cgi_owner.erase(fd);
}

/**
 * @brief Tue et debranche le CGI d'une connexion qu'on abandonne (client
 *        parti, timeout general de la connexion, ...). Les pipes encore
 *        ouverts sont retires de poll_fds immediatement ; le pid part dans
 *        _pending_reap pour un reapPending() ulterieur (jamais de waitpid()
 *        bloquant dans la boucle).
 * @param conn Connexion dont on abandonne le CGI en cours
 */
void	Server::killCgi(Connection& conn)
{
	if (conn.cgi_stdin_fd != -1)
		removeCgiPipe(conn.cgi_stdin_fd);
	if (conn.cgi_stdout_fd != -1)
		removeCgiPipe(conn.cgi_stdout_fd);
	if (conn.cgi_pid != -1)
	{
		kill(conn.cgi_pid, SIGKILL);
		_pending_reap.push_back(conn.cgi_pid);
		conn.cgi_pid = -1;
	}
}

/**
 * @brief Parcourt les connexions CGI_RUNNING : fait avancer celles dont les
 *        deux pipes sont deja fermes (cgi_handler::finish()), tue celles
 *        qui ont depasse conn.cgi_deadline (cgi_handler::abortTimeout()).
 *        Tourne a chaque tour de boucle (voir ServerLoop.cpp::runOnce),
 *        pas seulement quand poll() signale quelque chose de neuf : un
 *        CGI dont les pipes viennent de fermer peut rester un instant
 *        non-reapable (voir cgi_handler::finish()), et un CGI qui depasse
 *        juste son delai n'a rien de nouveau a signaler sur ses fd.
 */
void	Server::sweepCgi(void)
{
	ConnMap::iterator	it;
	pid_t				pending;

	for (it = _connections.begin(); it != _connections.end(); ++it)
	{
		Connection&	conn = it->second;

		if (conn.state != CGI_RUNNING || conn.cgi_pid == -1)
			continue ;
		if (cgi_handler::isDone(conn))
		{
			pending = 0;
			if (cgi_handler::finish(conn, pending))
			{
				if (pending != 0)
					_pending_reap.push_back(pending);
				setPollEvents(conn.fd, POLLOUT);
			}
		}
		else if (time(NULL) >= conn.cgi_deadline)
		{
			// abortTimeout() resets conn.cgi_stdin_fd/cgi_stdout_fd to -1
			// itself and expects the caller to have captured the real fd
			// numbers beforehand -- see its doc comment in CgiHandler.hpp.
			int	stdin_fd = conn.cgi_stdin_fd;
			int	stdout_fd = conn.cgi_stdout_fd;

			pending = cgi_handler::abortTimeout(conn);
			if (stdin_fd != -1)
				removeCgiPipe(stdin_fd);
			if (stdout_fd != -1)
				removeCgiPipe(stdout_fd);
			if (pending != 0)
				_pending_reap.push_back(pending);
			setPollEvents(conn.fd, POLLOUT);
		}
	}
}

/**
 * @brief Reap non-bloquant, au mieux, des pids de CGI tues/finis
 *        Jamais de waitpid(..., 0) : un pid qui n'est pas encore reapable
 *        reste dans _pending_reap pour le prochain tour.
 */
void	Server::reapPending(void)
{
	size_t	k = 0;

	while (k < _pending_reap.size())
	{
		if (cgi_handler::reapIfExited(_pending_reap[k]))
			_pending_reap.erase(_pending_reap.begin() + static_cast<long>(k));
		else
			++k;
	}
}
