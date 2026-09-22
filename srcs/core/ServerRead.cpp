#include "core/Server.hpp"

#include <ctime>
#include <iostream>
#include <unistd.h>

namespace {
const size_t READ_CHUNK = 65536;
}

/**
 * @brief Lit un morceau depuis un client pret en lecture
 *        L'ajoute a conn.read_buffer, rafraichit conn.last_activity, puis demande a
 *        try_parse_request() si une requete complete est maintenant
 *        disponible. En cas de succes (une requete complete OU une erreur de
 *        parsing try_parse_request() signale les deux en renvoyant true,
 *        voir son propre commentaire de doc) la connexion passe a
 *        PROCESSING, decide de la reponse,
 *        y compris pour le cas d'erreur.
 * @param conn Connexion client que poll() vient de signaler prete
 *             (POLLIN et/ou POLLHUP, voir ServerLoop.cpp::handleClientEvent)
 */
void	Server::handleRead(Connection& conn)
{
	char	chunk[READ_CHUNK];
	ssize_t	n;

	n = read(conn.fd, chunk, sizeof(chunk));
	if (n > 0)
	{
		conn.read_buffer.append(chunk, static_cast<size_t>(n));
		conn.last_activity = time(NULL);
		if (try_parse_request(conn))
			conn.state = PROCESSING;
		return ;
	}
	if (n == 0)
	{
		conn.state = DONE;
		return ;
	}
	std::cerr << "[handleRead] fd " << conn.fd << " : lecture impossible -> fermeture" << std::endl;
	conn.state = DONE;
}
