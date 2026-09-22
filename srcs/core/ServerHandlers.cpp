#include "core/Server.hpp"

void	Server::handleProcessing(Connection& conn)
{
	handle_request(conn);
	if (conn.state != CGI_RUNNING)
	{
		conn.state = WRITING_RESPONSE;
		setPollEvents(conn.fd, POLLOUT);
	}
}
