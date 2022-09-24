#ifdef _MSC_VER
#pragma warning (disable : 4786)
#endif

#include <winsock2.h>
#include "ConManager.h"

extern FILE *logfile;
extern void l ();
extern void u ();
extern char *ip2str(unsigned long ip);

void simple_luna_packet(luna_packet *lp, int type, int id)
{
	ZeroMemory(lp, sizeof(luna_packet));
	lp->header = PACKET_HEADER(type, id);
}

/////////////////////////////////////////////////
// Connection

Connection::Connection (ConManager *parent, SOCKET *sock, SOCKADDR_IN peer, ReceiveCallback callback)
{
	SECURITY_ATTRIBUTES sa;
	ZeroMemory(&sa, sizeof(sa));
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;
	sa.nLength = sizeof(sa);

	this->parent = parent;
	this->sock = sock;
	this->peer = peer;
	this->callback = callback;
	this->sem_ok = CreateSemaphore(&sa, 0, 1, NULL);
	this->sem_recv = CreateSemaphore(&sa, 0, LONG_MAX, NULL);
	this->sem_send_ready = CreateSemaphore(&sa, 1, 1, NULL);
	this->mutex = CreateMutex(&sa, FALSE, NULL);
	this->peer_addr = peer.sin_addr.s_addr;
	this->last_seen = GetTickCount();
	this->expected_l_id = 1;
	this->expected_r_id = 1;
	this->last_id = 1;
	this->async = 0;
	this->run = false;
	this->closed = false;
	this->packets = new std::queue<void *>();
}

void Connection::receive (void *packet)
{
	luna_packet *lp = (luna_packet *)packet; // header is the same for luna_packet and luna_jumbo_packet, so either is fine
	if (DEBUG) { l(); fprintf(logfile, "Receipt packet at %08x from %s: %04x %02u\n", lp, ip2str(peer_addr), PACKET_ID(*lp), PACKET_TYPE_LOW(*lp)); u(); }
	if (closed)
	{
		free(packet);
		return;
	}
	last_seen = GetTickCount();
	if (PACKET_IS(*lp, PACKET_TYPE_IGNOR))
	{
		free(packet);
		return;
	}
	if (DEBUG) { l(); fprintf(logfile, "Receipt packet accepted at %08x from %s: %04x %02u\n", lp, ip2str(peer_addr), PACKET_ID(*lp), PACKET_TYPE_LOW(*lp)); u(); }
	if (!async && !PACKET_IS(*lp, PACKET_TYPE_ERROR) && !PACKET_IS(*lp, PACKET_TYPE_TOUT))
	{
		if (PACKET_IS(*lp, PACKET_TYPE_OK))
		{
			WaitForSingleObject(mutex, INFINITE);
			if (PACKET_ID(*lp) == expected_l_id)
			{
				int tmp = expected_l_id;
				expected_l_id++;
				ReleaseSemaphore(sem_send_ready, 1, NULL);
				ReleaseMutex(mutex);
				if (DEBUG) { l(); fprintf(logfile, "Got OK for packet: %04x %02u\n", PACKET_ID(*lp), PACKET_TYPE_LOW(*lp)); u(); }
				parent->packet_done(this, tmp);
				ReleaseSemaphore(sem_ok, 1, NULL);
				WaitForSingleObject(mutex, INFINITE);
			}
			ReleaseMutex(mutex);
			free(packet);
		}
		else
		{
			luna_packet ok;

			if (!run && PACKET_IS(*lp, PACKET_TYPE_ALIVE))
			{
				packets->push(packet);
				ReleaseSemaphore(sem_recv, 1, NULL);
				return;
			}

			if ((last_id != 0xffff && PACKET_ID(*lp) < last_id) || (last_id == 0xffff && PACKET_ID(*lp) > 0xf000))
			{
				if (DEBUG) { l(); fprintf(logfile, "Discarding packet, last %04x, expecting local %04x and %04x remote: %04x %02u\n", last_id, expected_l_id, expected_r_id, PACKET_ID(*lp), PACKET_TYPE_LOW(*lp)); u(); }
				free(packet);
				return;
			}
			last_id = PACKET_ID(*lp);

			simple_luna_packet(&ok, PACKET_TYPE_OK, PACKET_ID(*lp));
			send_raw(&ok, sizeof(ok));
	
			WaitForSingleObject(mutex, INFINITE);
			if (DEBUG) { l(); fprintf(logfile, "Processing regular packet, expecting remote %04x: %04x %02u\n", expected_r_id, PACKET_ID(*lp), PACKET_TYPE_LOW(*lp)); u(); }
			if (PACKET_ID(*lp) == expected_r_id)
			{
				if (DEBUG) { l(); fprintf(logfile, "Enqueued received packet: %04x %02u\n", PACKET_ID(*lp), PACKET_TYPE_LOW(*lp)); u(); }
				packets->push(packet);
				expected_r_id++;
				ReleaseSemaphore(sem_recv, 1, NULL);
			}
			else if (PACKET_IS(*lp, PACKET_TYPE_RUN))
			{
				run = true;
				free(packet);
			}
			else
				free(packet);
			ReleaseMutex(mutex);
		}
	}
	else
	{
		AsyncPacket ap = {peer_addr, packet};
		callback(&ap); // this could be made a thread
		if (!PACKET_IS(*lp, PACKET_TYPE_ERROR))
			free(packet);
	}
}

bool Connection::wait_ok ()
{
	if (closed)
		return false;
	if (async || WaitForSingleObject(sem_ok, CONNECTION_TIMEOUT) == WAIT_OBJECT_0)
		return true;

	timeout();
	return false;
}

bool Connection::retrieve (void *packet, int packet_length)
{
	luna_packet *lp;
	if (closed)
		return false;
	if (async)
		return false;
	if (WaitForSingleObject(sem_recv, CONNECTION_TIMEOUT) == WAIT_OBJECT_0)
	{
		WaitForSingleObject(mutex, INFINITE);
		lp = (luna_packet *)packets->front();
		if (DEBUG) { l(); fprintf(logfile, "Retrieving packet at %08x from %s: %04x %02u\n", lp, ip2str(peer_addr), PACKET_ID(*lp), PACKET_TYPE_LOW(*lp)); u(); }
		memcpy(packet, packets->front(), packet_length);
		free(packets->front());
		packets->pop();
		ReleaseMutex(mutex);
		return true;
	}

	timeout();
	return false;
}

void Connection::set_async ()
{
	luna_packet *lp;
	async = true;

	// clean up sync packets
	do {} while (WaitForSingleObject(sem_recv, 0) == WAIT_OBJECT_0);
	WaitForSingleObject(mutex, INFINITE);
	while (packets != NULL && !packets->empty())
	{
		lp = (luna_packet *)packets->front();
		if (DEBUG) { l(); fprintf(logfile, "Freeing packet (set async) at %08x from %s: %04x %02u\n", lp, ip2str(peer_addr), PACKET_ID(*lp), PACKET_TYPE_LOW(*lp)); u(); }
		free(packets->front());
		packets->pop();
	}
	ReleaseMutex(mutex);
}

void Connection::disconnect_nobye ()
{
	disconnect_nobye(0);
}

void Connection::disconnect_nobye (int type)
{
	luna_packet *lp;
	if (closed)
		return;

	WaitForSingleObject(mutex, INFINITE);
	while (packets != NULL && !packets->empty())
	{
		lp = (luna_packet *)packets->front();
		if (DEBUG) { l(); fprintf(logfile, "Freeing packet at %08x from %s: %04x %02u\n", lp, ip2str(peer_addr), PACKET_ID(*lp), PACKET_TYPE_LOW(*lp)); u(); }
		free(lp);
		if (DEBUG) { l(); fprintf(logfile, "Freed packet."); u(); }
		packets->pop();
	}
	if (packets != NULL)
		delete packets;
	packets = NULL;
	CloseHandle(sem_ok);
	CloseHandle(sem_recv);
	closed = true;
	ReleaseMutex(mutex);
	CloseHandle(mutex);

	if (!type)
		parent->disconnect(peer_addr);
}

void Connection::disconnect ()
{
	disconnect(0);
}

void Connection::disconnect (int type)
{
	luna_packet lp;

	if (closed)
		return;

	simple_luna_packet(&lp, PACKET_TYPE_BYE, 0);
	send_raw(&lp, sizeof(lp));
	send_raw(&lp, sizeof(lp));
	send_raw(&lp, sizeof(lp));

	disconnect_nobye(type);
}

void Connection::timeout ()
{
	luna_packet lp;
	AsyncPacket ap;

	if (closed)
		return;

	simple_luna_packet(&lp, PACKET_TYPE_TOUT, 0);
	ap.peer = peer_addr;
	ap.packet = &lp;
	callback(&ap);

	disconnect();
}

void Connection::check_timeout ()
{
	if (closed)
		return;
	if (GetTickCount() > last_seen + CONNECTION_TIMEOUT)
		timeout();
}

bool Connection::send_raw (void *packet, int packet_length)
{
	if (closed)
		return false;
	if (DEBUG) { l(); fprintf(logfile, "Sending raw packet to %s: %04x %02u\n", inet_ntoa(peer.sin_addr), PACKET_ID(*(luna_packet *)packet), PACKET_TYPE_LOW(*(luna_packet *)packet)); u(); }
	if (sendto(*sock, (char *)packet, packet_length, 0, (SOCKADDR *)&peer, sizeof(peer)) != SOCKET_ERROR)
		return true;

	if (DEBUG) { l(); fprintf(logfile, "Sending raw packet to %s failed.\n", inet_ntoa(peer.sin_addr)); u(); }
	luna_packet lp;
	AsyncPacket ap;
	simple_luna_packet(&lp, PACKET_TYPE_ERROR, 0);
	ap.peer = peer_addr;
	ap.packet = &lp;
	callback(&ap);
	disconnect_nobye();

	return false;
}

bool Connection::send (void *packet, int packet_length, int *id)
{
	if (id != NULL)
		*id = 0;

	if (async)
		return send_raw(packet, packet_length);

	if (WaitForSingleObject(sem_send_ready, CONNECTION_TIMEOUT) == WAIT_TIMEOUT)
		return false;

	luna_packet *lp = (luna_packet *)packet;
	WaitForSingleObject(mutex, INFINITE);
	lp->header = PACKET_HEADER(PACKET_TYPE(*lp), expected_l_id);
	if (id != NULL)
		*id = expected_l_id;
	ReleaseMutex(mutex);

	return send_raw(packet, packet_length);
}

bool Connection::sync_mode ()
{
	return !async;
}

void Connection::got_expected ()
{
	WaitForSingleObject(mutex, INFINITE);
	expected_r_id++;
	ReleaseMutex(mutex);
}


/////////////////////////////////////////////////
// ConManager

ConManager::ConManager ()
{
	sock = NULL;
	port = 0;
	connections = NULL;
	sync_packets = NULL;
	mutex = NULL;
	receiver = NULL;
	started = false;
	server = false;
	sender = NULL;
	checker = NULL;
	default_callback = NULL;
	accepted = 0;
	accept_rc = NULL;
	sem_can_accept = NULL;
	sem_accepted = NULL;
}

// bounce from CreateThread C call back to C++ code
void receive_thread_caller (void *p) { ((ConManager *)p)->receive_thread(); }
void send_thread_caller (void *p) { ((ConManager *)p)->send_thread(); }
void check_thread_caller (void *p) { ((ConManager *)p)->check_thread(); }

void ConManager::init (SOCKET *sock, int port, DefaultCallback default_callback)
{
	SECURITY_ATTRIBUTES sa;
	ZeroMemory(&sa, sizeof(sa));
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;
	sa.nLength = sizeof(sa);

	this->sock = sock;
	this->port = port;
	this->default_callback = default_callback;
	server = false;
	connections = new ConnectionMap();
	sync_packets = new std::list<QueuedPacket>();
	accepted = 0;
	accept_rc = NULL;
	mutex = CreateMutex(&sa, FALSE, NULL);
	sem_can_accept = CreateSemaphore(&sa, 0, 1, NULL);
	sem_accepted = CreateSemaphore(&sa, 0, LONG_MAX, NULL);
	sender = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)send_thread_caller, (void *)this, 0, NULL);
	checker = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)check_thread_caller, (void *)this, 0, NULL);
	started = false;
}

void ConManager::start_receiver ()
{
	started = true;
	receiver = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)receive_thread_caller, (void *)this, 0, NULL);
}

void ConManager::clear ()
{
	TerminateThread(receiver, 0);
	TerminateThread(checker, 0);
	TerminateThread(sender, 0);
	CloseHandle(receiver);
	CloseHandle(sender);
	CloseHandle(checker);
	CloseHandle(mutex);
	CloseHandle(sem_can_accept);
	CloseHandle(sem_accepted);
	sock = NULL;
	port = 0;
	server = false;
	started = false;
	default_callback = NULL;
	accept_rc = NULL;
	accepted = 0;
	mutex = NULL;
	sem_can_accept = NULL;
	sem_accepted = NULL;
	receiver = NULL;
	sender = NULL;
	checker = NULL;

	std::list<Connection *> c_list;
	c_list.clear();
	for (ConnectionMap::iterator i = connections->begin(); i != connections->end(); ++i)
	{
		c_list.push_back(i->second);
	}
	for (std::list<Connection *>::iterator k = c_list.begin(); k != c_list.end(); ++k)
	{
		(*k)->disconnect_nobye();
	}
	delete connections;
	connections = NULL;
	for (std::list<QueuedPacket>::iterator j = sync_packets->begin(); j != sync_packets->end(); )
	{
		free(j->packet);
		j = sync_packets->erase(j);
	}
	delete sync_packets;
	sync_packets = NULL;
}

void ConManager::receive_thread ()
{
	luna_jumbo_packet *ljp;
	SOCKADDR_IN from;
	SOCKADDR_IN sa;
	int from_size = sizeof(from);
	int ret;

	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(port);

	for (;;)
	{
		ljp = (luna_jumbo_packet *)calloc(sizeof(luna_jumbo_packet), 1);
		if (DEBUG) { l(); fprintf(logfile, "Listening for packet.\n"); u(); }
		ret = recvfrom(*sock, (char *)ljp, sizeof(luna_jumbo_packet), 0, (SOCKADDR *)&from, &from_size);

		// check socket errors
		if (ret == SOCKET_ERROR)
		{
			free(ljp);
			l();
			printf("Couldn't read from socket: ");
			switch (WSAGetLastError())
			{
			case WSANOTINITIALISED: printf("WSANOTINITIALISED\n"); break;
			case WSAENETDOWN: printf("WSAENETDOWN\n"); break;
			case WSAEFAULT: printf("WSAEFAULT\n"); break;
			case WSAEINTR: printf("WSAEINTR\n"); break;
			case WSAEINPROGRESS: printf("WSAEINPROGRESS\n"); break;
			case WSAEINVAL: printf("WSAEINVAL\n"); break;
			case WSAEISCONN: printf("WSAEISCONN\n"); break;
			case WSAENETRESET: printf("WSAENETRESET\n"); break;
			case WSAENOTSOCK: printf("WSAENOTSOCK\n"); break;
			case WSAEOPNOTSUPP: printf("WSAEOPNOTSUPP\n"); break;
			case WSAESHUTDOWN: printf("WSAESHUTDOWN\n"); break;
			case WSAEWOULDBLOCK: printf("WSAEWOULDBLOCK\n"); break;
			case WSAEMSGSIZE: printf("WSAEMSGSIZE\n"); break;
			case WSAETIMEDOUT: printf("WSAETIMEDOUT\n"); break;
			case WSAECONNRESET: /*printf("WSAECONNRESET\n");*/ printf("Connection reset by peer.\n"); /*return;*/ break;
			default: printf("%u\n", WSAGetLastError()); break;
			}
			u();

			if (WaitForSingleObject(sem_can_accept, 0) == WAIT_OBJECT_0)
			{
				accepted = 0;
				ReleaseSemaphore(sem_accepted, 1, NULL);
			}

			// this sucks
			return;

#if 0
			if (!server)
			{
				std::list<Connection *> c_list;
				luna_packet error;
				simple_luna_packet(&error, PACKET_TYPE_ERROR, 0);
				WaitForSingleObject(mutex, INFINITE);
				c_list.clear();
				for (ConnectionMap::iterator i = connections->begin(); i != connections->end(); ++i)
				{
					c_list.push_back(i->second);
				}
				for (std::list<Connection *>::iterator j = c_list.begin(); j != c_list.end(); ++j)
				{
					(*j)->receive((void *)&error);
					(*j)->disconnect_nobye();
				}
				ReleaseMutex(mutex);

				return; // give up for now
			}
			else
			{
				shutdown(*sock, SD_RECEIVE);
				closesocket(*sock);
				//Sleep(20);
				*sock = socket(AF_INET, SOCK_DGRAM, 0);
				bind(*sock, (SOCKADDR *)&sa, sizeof(sa));
			}
#endif

			continue;
		}
		if (DEBUG) { l(); fprintf(logfile, "Received (%u) packet from %s: %04x %02u\n", ret, inet_ntoa(from.sin_addr), PACKET_ID(*ljp), PACKET_TYPE_LOW(*ljp)); u(); }

		// check version
		if (!PACKET_GOOD_VERSION(*ljp))
		{
			l();
			printf("Incompatible protocol version %u in packet from %s.\n", PACKET_VERSION(*ljp), inet_ntoa(from.sin_addr));
			u();
			free(ljp);
			continue;
		}

		// check size
		if (!(ret == sizeof(luna_packet) || ret == sizeof(luna_jumbo_packet)))
		{
			l();
			printf("Invalid packet from %s. Expected length %u or %u, got %u.\n", inet_ntoa(from.sin_addr), sizeof(luna_packet), sizeof(luna_jumbo_packet), ret);
			u();
			free(ljp);
			continue;
		}

		// check valid source address
		if (from.sin_addr.s_addr == 0)
		{
			l();
			printf("Invalid source address %s in datagram.\n", inet_ntoa(from.sin_addr));
			u();
			free(ljp);
			continue;
		}

		// ensure JUMBO type for jumbo sized packets
		if (ret == sizeof(luna_jumbo_packet) && !PACKET_IS((*ljp), PACKET_TYPE_JUMBO))
		{
			l();
			printf("Invalid big packet from %s.\n", inet_ntoa(from.sin_addr));
			u();
			free(ljp);
			continue;
		}

		// check for known connection to associate packet with
		if (connections->count(from.sin_addr.s_addr))
			(*connections)[from.sin_addr.s_addr]->receive(ljp); // packet will get freed in Connection or ConManager receive function
		else
		{
			// check for new connection from client to host
			if (PACKET_IS(*ljp, PACKET_TYPE_HELLO))
			{
				// waiting for new client connection?
				WaitForSingleObject(mutex, INFINITE);

				if (accepted == from.sin_addr.s_addr)
				{
					// resend ok
					luna_packet ok;
					simple_luna_packet(&ok, PACKET_TYPE_OK, 1);
					if (connections->count(from.sin_addr.s_addr))
						(*connections)[from.sin_addr.s_addr]->send_raw((void *)&ok, sizeof(ok));
				}
				else if (WaitForSingleObject(sem_can_accept, 0) == WAIT_OBJECT_0)
				{
					// accept
					luna_packet ok;
					Sleep(10);
					accepted = from.sin_addr.s_addr;
					ReleaseMutex(mutex);
					register_connection(from, accept_rc);
					simple_luna_packet(&ok, PACKET_TYPE_OK, 1);
					if (connections->count(from.sin_addr.s_addr))
					{
						(*connections)[from.sin_addr.s_addr]->send_raw((void *)&ok, sizeof(ok));
						(*connections)[from.sin_addr.s_addr]->got_expected();
					}
					ReleaseSemaphore(sem_accepted, 1, NULL);
				}
				else
				{
					// deny access
					ReleaseMutex(mutex);
					luna_packet lp;
					simple_luna_packet(&lp, PACKET_TYPE_OK, PACKET_ID(*ljp));
					sendto(*sock, (char *)&lp, sizeof(lp), 0, (SOCKADDR *)&from, from_size);
					simple_luna_packet(&lp, PACKET_TYPE_DENY, PACKET_ID(*ljp) + 1);
					sendto(*sock, (char *)&lp, sizeof(lp), 0, (SOCKADDR *)&from, from_size);
				}
				free(ljp);
				continue;
			}

			// default callback otherwise
			if (!default_callback(from, ljp))
				free(ljp);
		}
	}
}

void ConManager::send_thread ()
{
	int now;
	for (;;)
	{
		Sleep(SYNC_RESEND/2);
		WaitForSingleObject(mutex, INFINITE);
		now = GetTickCount();
		for (std::list<QueuedPacket>::iterator j = sync_packets->begin(); j != sync_packets->end(); )
		{
			QueuedPacket qp = *j;
			if (qp.last_sent <= now - SYNC_RESEND)
			{
				qp.last_sent = SYNC_RESEND;
				qp.connection->send_raw(qp.packet, qp.packet_size);
			}
			++j;
		}
		ReleaseMutex(mutex);
	}
}

void ConManager::check_thread ()
{
	std::list<Connection *> c_list;
	for (;;)
	{
		Sleep(TIMEOUT_CHECK);
		WaitForSingleObject(mutex, INFINITE);
		c_list.clear();
		for (ConnectionMap::iterator i = connections->begin(); i != connections->end(); ++i)
		{
			c_list.push_back(i->second);
		}
		for (std::list<Connection *>::iterator j = c_list.begin(); j != c_list.end(); ++j)
		{
			(*j)->check_timeout();
		}
		ReleaseMutex(mutex);
	}
}

void ConManager::rereceive (unsigned long peer, void *packet)
{
	Connection *c;
	if (connections->count(peer))
	{
		c = (*connections)[peer];
		c->receive(packet);
	}
}

bool ConManager::receive_generic (void *packet, unsigned long peer, int packet_size)
{
	Connection *c;
	bool ok;

	if (!connections->count(peer))
		return false;

	c = (*connections)[peer];
	ok = c->retrieve(packet, packet_size);
	
	if (!ok)
	{
		ZeroMemory(packet, packet_size);
		return false;
	}

	if (DEBUG) { l(); fprintf(logfile, "Received generic packet: %04x %02u\n", PACKET_ID(*(luna_packet *)packet), PACKET_TYPE_LOW(*(luna_packet *)packet)); u(); }

	return true;
}

bool ConManager::receive (luna_packet *packet, unsigned long peer)
{
	return receive_generic((void *)packet, peer, sizeof(luna_packet));
}

bool ConManager::receive_jumbo (luna_jumbo_packet *packet, unsigned long peer)
{
	return receive_generic((void *)packet, peer, sizeof(luna_jumbo_packet));
}

void ConManager::register_connection (SOCKADDR_IN peer, ReceiveCallback callback)
{
	Connection *c;

	WaitForSingleObject(mutex, INFINITE);
	if (connections->count(peer.sin_addr.s_addr))
	{
		ReleaseMutex(mutex);
		return;
	}

	c = new Connection(this, sock, peer, callback);
	(*connections)[peer.sin_addr.s_addr] = c;
	ReleaseMutex(mutex);
}

bool ConManager::send_raw (void *packet, unsigned long peer, int packet_size)
{
	Connection *c;
	if (!connections->count(peer))
		return false;
	c = (*connections)[peer];
	return c->send_raw(packet, packet_size);
}

bool ConManager::send_generic (void *packet, unsigned long peer, int packet_size)
{
	Connection *c;
	char *buf;
	bool ok;

	if (!connections->count(peer))
		return false;

	if (DEBUG) { l(); fprintf(logfile, "Send generic to %u: %04x %02u\n", peer, PACKET_ID(*(luna_packet *)packet), PACKET_TYPE_LOW(*(luna_packet *)packet)); u(); }
	c = (*connections)[peer];
	if (DEBUG) { l(); fprintf(logfile, "Got connection for generic.\n"); u(); }

	if (c->sync_mode())
	{
		QueuedPacket qp;

		if (DEBUG) { l(); fprintf(logfile, "Using sync mode for generic.\n"); u(); }

		buf = (char *)malloc(packet_size);
		memcpy((void *)buf, packet, packet_size);
		qp.last_sent = GetTickCount();
		qp.connection = c;
		qp.packet = (void *)buf;
		qp.packet_size = packet_size;

		if (DEBUG) { l(); fprintf(logfile, "Prepared packet for generic. Sending.\n"); u(); }
		ok = c->send(qp.packet, packet_size, &(qp.id));

		if (ok)
		{
			WaitForSingleObject(mutex, INFINITE);
			sync_packets->push_back(qp);
			ReleaseMutex(mutex);

			if (!started)
				start_receiver();
			ok = c->wait_ok();
		}

		return ok;
	}

	if (DEBUG) { l(); fprintf(logfile, "Using async mode for generic.\n"); u(); }
	ok = c->send(packet, packet_size, NULL);
	if (ok && !started)
		start_receiver();

	return ok;
}

bool ConManager::send (luna_packet *packet, unsigned long peer)
{
	return send_generic((void *)packet, peer, sizeof(luna_packet));
}

bool ConManager::send_jumbo (luna_jumbo_packet *packet, unsigned long peer)
{
	return send_generic((void *)packet, peer, sizeof(luna_jumbo_packet));
}

void ConManager::packet_done (Connection *connection, int id)
{
	WaitForSingleObject(mutex, INFINITE);
	for (std::list<QueuedPacket>::iterator i = sync_packets->begin(); i != sync_packets->end(); )
	{
		if (i->connection == connection && i->id == id)
		{
			free(i->packet);
			i = sync_packets->erase(i);
		}
		else
			++i;
	}
	ReleaseMutex(mutex);
}

void ConManager::set_async (unsigned long peer)
{
	Connection *c;
	if (connections->count(peer))
	{
		c = (*connections)[peer];
		c->set_async();
	}
}

void ConManager::disconnect (unsigned long peer)
{
	Connection *c = NULL;
	WaitForSingleObject(mutex, INFINITE);
	for (ConnectionMap::iterator i = connections->begin(); i != connections->end(); )
	{
		if (i->first == peer)
		{
			if (DEBUG) { l(); fprintf(logfile, "Disconnect %s.\n", ip2str(peer)); u(); }
			i->second->disconnect(1);
			if (DEBUG) { l(); fprintf(logfile, "Disconnected, freeing %s.\n", ip2str(peer)); u(); }
			c = i->second;
			delete (i->second);
			if (DEBUG) { l(); fprintf(logfile, "Freed, unmapping %s.\n", ip2str(peer)); u(); }
			connections->erase(i++);
			if (DEBUG) { l(); fprintf(logfile, "Unmapped %s.\n", ip2str(peer)); u(); }
		}
		else
			++i;
	}
	for (std::list<QueuedPacket>::iterator j = sync_packets->begin(); j != sync_packets->end(); )
	{
		if (j->connection == c)
		{
			free(j->packet);
			j = sync_packets->erase(j);
		}
		else
			++j;
	}
	ReleaseMutex(mutex);
}

void ConManager::disconnect_nobye (unsigned long peer)
{
	Connection *c = NULL;
	WaitForSingleObject(mutex, INFINITE);
	for (ConnectionMap::iterator i = connections->begin(); i != connections->end(); )
	{
		if (i->first == peer)
		{
			c = i->second;
			i->second->disconnect_nobye(1);
			delete (i->second);
			connections->erase(i++);
		}
		else
			++i;
	}
	for (std::list<QueuedPacket>::iterator j = sync_packets->begin(); j != sync_packets->end(); )
	{
		if (j->connection == c)
		{
			free(j->packet);
			j = sync_packets->erase(j);
		}
		else
			++j;
	}
	ReleaseMutex(mutex);
}

unsigned long ConManager::accept (ReceiveCallback callback)
{
	accepted = 0;
	accept_rc = callback;
	server = true;
	ReleaseSemaphore(sem_can_accept, 1, NULL);
	if (!started)
		start_receiver();
	WaitForSingleObject(sem_accepted, INFINITE);
	return accepted;
}
