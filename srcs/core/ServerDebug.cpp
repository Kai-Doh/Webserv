#include "core/Server.hpp"

#include <string>

/**
 * @brief Transforme un masque d'evenements poll en texte lisible
 * @param events Masque (POLLIN, POLLOUT ou les deux)
 * @return "POLLIN", "POLLOUT", "POLLIN|POLLOUT" ou "none"
 */
static std::string	events_to_string(short events)
{
	std::string	s;

	if (events & POLLIN)
		s += "POLLIN";
	if (events & POLLOUT)
		s += (s.empty() ? "POLLOUT" : "|POLLOUT");
	return (s.empty() ? "none" : s);
}

/**
 * @brief Transforme un etat de connexion en texte lisible
 * @param state Valeur de l'enum ConnState
 * @return Le nom de l'etat
 */
static const char	*state_to_string(ConnState state)
{
	switch (state)
	{
		case READING_REQUEST:	return ("READING_REQUEST");
		case PROCESSING:		return ("PROCESSING");
		case CGI_RUNNING:		return ("CGI_RUNNING");
		case WRITING_RESPONSE:	return ("WRITING_RESPONSE");
		case DONE:				return ("DONE");
	}
	return ("UNKNOWN");
}

// Debug

/**
 * @brief Affiche poll_fds et connections (aide au debug, utilisee par main pour l'instant)
 * @param os Flux sur lequel afficher
 */
void	Server::dump(std::ostream& os) const
{
	os << "poll_fds (" << _poll_fds.size() << ") :" << std::endl;
	for (size_t i = 0; i < _poll_fds.size(); ++i)
	{
		os << "  [" << i << "] fd=" << _poll_fds[i].fd
			<< "  events=" << events_to_string(_poll_fds[i].events);
		ListenMap::const_iterator l = _listen_fds.find(_poll_fds[i].fd);
		if (l != _listen_fds.end())
			os << "  (ecoute " << (l->second->getHost().empty() ? "*" : l->second->getHost())
				<< ":" << l->second->getPort() << ")";
		os << std::endl;
	}
	os << "connections (" << _connections.size() << ") :" << std::endl;
	for (ConnMap::const_iterator it = _connections.begin(); it != _connections.end(); ++it)
		os << "  fd=" << it->first << "  state=" << state_to_string(it->second.state)
			<< "  read=" << it->second.read_buffer.size()
			<< "o  write=" << it->second.bytes_written << "/"
			<< it->second.write_buffer.size() << "o" << std::endl;
}
