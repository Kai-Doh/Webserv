#include "core/Server.hpp"

#include <ctime>
#include <iostream>
#include <unistd.h>

/**
 * @brief Ecrit autant de write_buffer que le socket accepte pour l'instant
 *        , puis decide de la suite write_buffer a ete rempli par handle_request()
 * @param conn Connexion client que poll() vient de signaler prete en ecriture
 *             (POLLOUT et/ou POLLHUP, voir ServerLoop.cpp::handleClientEvent)
 */
void	Server::handleWrite(Connection& conn)
{
	size_t	remaining = conn.write_buffer.size() - conn.bytes_written;
	ssize_t	n;

	n = write(conn.fd, conn.write_buffer.data() + conn.bytes_written, remaining);
	if (n > 0)
	{
		conn.bytes_written += static_cast<size_t>(n);
		conn.last_activity = time(NULL);
		if (conn.bytes_written == conn.write_buffer.size())
		{
			if (!conn.keep_alive)
			{
				conn.state = DONE;
				return ;
			}
			resetConnectionForReuse(conn);
			setPollEvents(conn.fd, POLLIN);
			if (try_parse_request(conn))
			{
				conn.state = PROCESSING;
				handleProcessing(conn);
			}
			return ;
		}
		return ;
	}
	std::cerr << "[handleWrite] fd " << conn.fd << " : ecriture impossible -> fermeture" << std::endl;
	conn.state = DONE;
}
