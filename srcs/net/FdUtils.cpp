#include "net/FdUtils.hpp"

#include <fcntl.h>

/**
 * @brief Passe un descripteur de fichier en mode non-bloquant
 *        Le SEUL endroit ou fcntl() est appele : le sujet n'autorise que
 *        F_SETFL avec O_NONBLOCK (et FD_CLOEXEC).
 *        errno est laisse intact apres un echec, pour que l'appelant puisse
 *        le rapporter.
 * @param fd Descripteur de fichier a modifier
 * @return true en cas de succes, false si fcntl() a echoue
 */
bool	setFdNonBlocking(int fd)
{
	return (fcntl(fd, F_SETFL, O_NONBLOCK) != -1);
}
