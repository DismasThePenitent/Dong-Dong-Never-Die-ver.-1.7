#ifndef CONMANAGER_H
#define CONMANAGER_H

#include <map>
#include <list>
#include <queue>
#include <windows.h>
#include "LunaPort.h"

void simple_luna_packet(luna_packet *lp, int type, int id);

// notes:
// buffers for packets are at least sizeof(luna_jumbo_packet), so they can be casted as necessary
// caller of receive has to free the returned packet
// "sync" means used in a linear handshake sequence
// sending regular luna_packets to a spectator is ok during handshake, or for header-only messages
// DefaultCallback should launch a thread for spectator handshake

typedef struct _AsyncPacket
{
	unsigned long peer;
	void *packet;
} AsyncPacket;
typedef void (*ReceiveCallback)(AsyncPacket *packet);

class ConManager;

class Connection
{
	private:
		ConManager *parent;
		SOCKET *sock;
		SOCKADDR_IN peer;
		unsigned long peer_addr;
		DWORD last_seen;          // timestamp of last received packet (pre-discard)
		WORD expected_r_id;        // for incoming packets, increase on receive
		WORD expected_l_id;        // for outgoing packets and ok, increase on OK
		int last_id;              // discard all sync packets with ID lower to this, set when receiving
		bool async;               // initialize as false
		bool run;                 // used for spectators, received RUN packet
		bool closed;
		ReceiveCallback callback; // callback to call with peer/packet on async receive
		HANDLE mutex;             // used to sync list access
		HANDLE sem_ok;            // used to wait for OK on sending sync packets
		HANDLE sem_recv;          // used to wait for real packet
		HANDLE sem_send_ready;    // ready to send next sync packet
		std::queue<void *> *packets; // sync packets

		void timeout ();

	public:
		Connection (ConManager *parent, SOCKET *sock, SOCKADDR_IN peer, ReceiveCallback callback);
		void receive (void *packet); // do not call callback in sync mode, but send back OK on real packet. release mutex and increase expected_id on OK
		bool wait_ok ();
		bool sync_mode ();
		bool retrieve (void *packet, int packet_length); // get last sync packet
		void set_async ();
		void disconnect ();
		void disconnect (int type);
		void disconnect_nobye ();
		void disconnect_nobye (int type);
		void check_timeout (); // on timeout, if there is a callback, send it PACKET_TYPE_TOUT
		bool send_raw (void *packet, int packet_length);
		bool send (void *packet, int packet_length, int *id); // automatically set id for sync packets
		void got_expected ();
};

typedef std::map<const unsigned long, Connection *> ConnectionMap;
typedef bool (*DefaultCallback)(SOCKADDR_IN peer, void *packet); // called on receiving connection from unknown host outside accept

typedef struct _QueuedPacket
{
	DWORD last_sent;
	Connection *connection;
	void *packet; // has to be malloced/freed
	int id;
	int packet_size;
} QueuedPacket;

class ConManager // connection manager
{
	private:
		SOCKET *sock;
		int port;
		bool started;
		bool server;
		ConnectionMap *connections;
		std::list<QueuedPacket> *sync_packets; // pending sync packets
		HANDLE mutex;                          // for access to map and list
		HANDLE receiver;                       // thread for receiving packets
		HANDLE sender;                         // thread for sending sync packets
		HANDLE checker;                        // timeout checker thread, thread function takes connections pointer as argument
		DefaultCallback default_callback;      // mainly used for connecting spectators
		unsigned long accepted;                // address of accepted peer
		ReceiveCallback accept_rc;             // callback to be set for accepted connection
		HANDLE sem_can_accept;                 // semaphore to be released when accept is called
		HANDLE sem_accepted;                   // semaphore to be released when connection has been accepted

		bool send_generic (void *packet, unsigned long peer, int packet_size); // makes a copy of packet in sync mode
		bool receive_generic (void *packet, unsigned long peer, int packet_size);

	public:
		ConManager ();
		void init (SOCKET *sock, int port, DefaultCallback default_callback); // reverse of clear
		void start_receiver (); // start receiving packets, socket is bound, or first packet has been sent
		void clear (); // disconnect and deallocate all connections, stop checker, destroy mutex, destroy list

		void receive_thread ();
		void send_thread ();
		void check_thread ();

		bool receive (luna_packet *packet, unsigned long peer); // return NULL on timeout/error, calls default_callback on other peer and keeps trying
		bool receive_jumbo (luna_jumbo_packet *packet, unsigned long peer); // return NULL on timeout/error, calls default_callback on other peer and keeps trying
		bool send (luna_packet *packet, unsigned long peer); // makes a copy of packet in sync mode
		bool send_jumbo (luna_jumbo_packet *packet, unsigned long peer); // makes a copy of packet in sync mode
		bool send_raw (void *packet, unsigned long peer, int packet_size);
		void packet_done (Connection *connection, int id);
		void rereceive (unsigned long peer, void *packet);
		void register_connection (SOCKADDR_IN peer, ReceiveCallback callback);

		void set_async (unsigned long peer); // set connection to async mode
		void disconnect (unsigned long peer);
		void disconnect_nobye (unsigned long peer);

		// HELLO packets will be denied, unless accept() is currently active
		unsigned long accept (ReceiveCallback callback); // limits packet type to HELLO using first 4 bytes of packet, see receive, call callback on other stuff
};

#endif
