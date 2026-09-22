#ifndef SOCKET_HPP
# define SOCKET_HPP

# include <string>

class Socket
{
public:
	Socket(const std::string& host, int port);
	~Socket(void);

	void				setup(void);
	void				closeSocket(void);

	int					getFd(void) const;
	const std::string&	getHost(void) const;
	int					getPort(void) const;

private:
	Socket(const Socket& other);
	Socket&	operator=(const Socket& other);

	void				createSocket(void);
	void				setReuseAddr(void);
	void				setNonBlocking(void);
	void				bindAddress(void);
	void				startListening(void);
	std::string			addressLabel(void) const;

	std::string	_host;
	int			_port;
	int			_fd;
};

#endif
