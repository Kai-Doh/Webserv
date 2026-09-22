#include "core/Server.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <string>

// Duree maximale d'une attente dans poll()
static const int				POLL_TIMEOUT_MS = 1000;

// Leve par SIGINT / SIGTERM
static volatile sig_atomic_t	g_stop_requested = 0;

/**
 * @brief Handler de signal pour SIGINT / SIGTERM : se contente de lever le stop
 * @param sig Numero du signal (non utilise)
 */
static void	on_stop_signal(int sig)
{
	(void)sig;
	g_stop_requested = 1;
}

// Boucle principale

/**
 * @brief Configure les signaux du processus pour la boucle
 *        SIGINT / SIGTERM : demande a la boucle de s'arreter proprement (Ctrl+C, kill)
 *        SIGPIPE          : ignore, sinon ecrire vers un client deja parti tuerait tout le serveur
 */
void	Server::installSignalHandlers(void)
{
	signal(SIGINT, on_stop_signal);
	signal(SIGTERM, on_stop_signal);
	signal(SIGPIPE, SIG_IGN);
}

/**
 * @brief Fait tourner le serveur jusqu'a SIGINT / SIGTERM (la seule boucle poll())
 * @throw std::runtime_error en cas d'erreur irrecuperable de poll() / d'un socket d'ecoute
 */
void	Server::run(void)
{
	g_stop_requested = 0;
	installSignalHandlers();
	while (!g_stop_requested)
		runOnce(POLL_TIMEOUT_MS);
}

/**
 * @brief Un tour de boucle : un seul poll(), puis traitement de ce qui est pret.
 * @param timeout_ms Attente max en ms (-1 = indefiniment, 0 = ne pas attendre)
 * @throw std::runtime_error si poll() echoue pour une raison autre qu'un signal
 */
void	Server::runOnce(int timeout_ms)
{
	int	ready;
	int	saved_errno;

	if (_poll_fds.empty())
		throw std::logic_error("runOnce: nothing to poll");
	ready = poll(&_poll_fds[0], static_cast<nfds_t>(_poll_fds.size()), timeout_ms);
	saved_errno = errno;
	if (ready < 0)
	{
		if (saved_errno == EINTR)
			return ;
		throw std::runtime_error(std::string("poll: ") + strerror(saved_errno));
	}
	if (ready > 0)
		dispatchEvents();
	sweepTimeouts();
}

/**
 * @brief Parcourt poll_fds apres un reveil et route chaque fd pret
 *        - fd d'ecoute pret            -> acceptNewClient()
 *        - fd client avec un evenement -> handleClientEvent()
 *        - les clients passes a DONE sont retires APRES le parcours
 */
void	Server::dispatchEvents(void)
{
	std::vector<int>	to_close;
	size_t				count = _poll_fds.size();
	Connection			*conn;

	for (size_t i = 0; i < count; ++i)
	{
		int		fd = _poll_fds[i].fd;
		short	revents = _poll_fds[i].revents;

		if (revents == 0)
			continue ;
		if (isListenFd(fd))
		{
			if (revents & (POLLERR | POLLNVAL))
				throw std::runtime_error("listening socket in error");
			if (revents & POLLIN)
				acceptNewClient(fd);
			continue ;
		}
		conn = findConnection(fd);
		if (conn == NULL)
			continue ;
		handleClientEvent(*conn, revents);
		if (conn->state == DONE)
			to_close.push_back(fd);
	}
	for (size_t k = 0; k < to_close.size(); ++k)
		removeConnection(to_close[k]);
}

/**
 * @brief Decide quoi faire d'un client a partir de son etat ET de ce que poll() a signale.
 * @param conn La Connection du client
 * @param revents Evenements signales par poll() pour ce fd lors de ce reveil
 */
void	Server::handleClientEvent(Connection& conn, short revents)
{
	if (revents & (POLLERR | POLLNVAL))
	{
		conn.state = DONE;
		return ;
	}
	if (conn.state == READING_REQUEST && (revents & (POLLIN | POLLHUP)))
		handleRead(conn);
	if (conn.state == PROCESSING)
		handleProcessing(conn);
	if (conn.state == WRITING_RESPONSE && (revents & (POLLOUT | POLLHUP)))
		handleWrite(conn);
}
