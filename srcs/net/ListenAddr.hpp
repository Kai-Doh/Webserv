#ifndef LISTENADDR_HPP
# define LISTENADDR_HPP

# include <string>

struct ListenAddr
{
	std::string	host;
	int			port;

	ListenAddr(const std::string& h = "", int p = 0) : host(h), port(p) {}
};

#endif
