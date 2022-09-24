#ifndef LUNAPORT_H
#define LUNAPORT_H

#include <map>
#include <windows.h>

#ifdef __GNUC__
#define PACKED_STRUCT __attribute((packed))
#endif


// game settings
#define VERSION "r39"
#define GAME "ヴァンガードプリンセス.exe"
#define ALTGAME "game.exe"
#define PORT 7500
#define MS_PER_INPUT 10 // this is correct, don't believe it? measure it with r22 by recording a replay locally
#define SAFETY_DELAY 1  // an additional delay of 1 for safety
#define DEBUG 0


// things about the past
#define REPLAY_HEADER "VPR0"
#define REPLAY_HEADER_V2 "VPR1"
#define HISTORY_CHUNK_SIZE (2*1024*1024)


// general network defines and types ; expected bandwidth about 28160kbps + UDP/IP overhead in each direction, without spectators
#define PROTOCOL_VERSION 13          // version is X, everything else should be rejected with error
#define CONNECTION_TIMEOUT 10000     // 10 seconds without packet received means connection timed out
#define SYNC_RESEND 76               // resend handshake packets approx every Xms
#define TIMEOUT_CHECK 250            // check all connections for timeout every Xms
#define BACKUP_VALUES 8              // include last X inputs with each input packet
#define ALIVE_INTERVAL 100           // send keep alive every Xms
#define SPIKE_INTERVAL 2             // after waiting Xms, assume a lag spike and try compensating
#define NET_STRING_PERPACKET    (4+BACKUP_VALUES*2) // number of bytes that fit into luna_packet
#define NET_STRING_LENGTH       512  // max length of string transmitted over network
#define NET_STRING_BUFFER       513  // buffer size for receiving/sending network string, ensure this value never is smaller than before
#define PACKET_VERSION(lp)      (((lp).header & 0xFF000000) >> 24)
#define PACKET_GOOD_VERSION(lp) (PACKET_VERSION(lp) == PROTOCOL_VERSION)

#define PACKET_TYPE_SEED  (1  << 16) // value = random seed, can only be sent by host
#define PACKET_TYPE_PNUM  (2  << 16) // value = player number for client, always from host in response to HELLO
#define PACKET_TYPE_DELAY (3  << 16) // value = (delay_value & 0xff) | ((ping_in_ms & 0xffffff) << 8)
#define PACKET_TYPE_INPUT (4  << 16) // value = input from local player
#define PACKET_TYPE_PING  (5  << 16) // value = don't care
#define PACKET_TYPE_HELLO (6  << 16) // value = don't care, always from client
#define PACKET_TYPE_BYE   (7  << 16) // value = don't care
#define PACKET_TYPE_DENY  (8  << 16) // value = don't care
#define PACKET_TYPE_JUMBO (9  << 16) // this is a jumbo packet with JUMBO_VALUES values, only sent to spectators
#define PACKET_TYPE_SPECT (10 << 16) // never sent by playing peers. client wants to spectate/resend. value = position in stream requested as starting point
#define PACKET_TYPE_OK    (11 << 16) // value = don't care, sent by client during handshake for synchronization
#define PACKET_TYPE_TEXT  (12 << 16) // value+backup = char *
#define PACKET_TYPE_DONE  (13 << 16) // value = don't care, finished sending text
#define PACKET_TYPE_AGAIN (14 << 16) // only set low_id and high_id of range to be resent (max: 2*d)
#define PACKET_TYPE_RUN   (15 << 16) // value = don't care, sent from host/client to spectator when game starts
#define PACKET_TYPE_ALIVE (16 << 16) // value = don't care, sent from host/client to spectator before game starts
#define PACKET_TYPE_TOUT  (17 << 16) // fake packet sent to packet handler when a timeout is detected. should work like bye, but with specific message
#define PACKET_TYPE_IGNOR (18 << 16) // value = don't care, sent from spec to host/client, is discarded in receive function
#define PACKET_TYPE_ERROR (19 << 16) // value = don't care, works like BYE with special message

#define PACKET_TYPE(lp)                 ((lp).header & 0x00ff0000)
#define PACKET_TYPE_LOW(lp)             (PACKET_TYPE(lp) >> 16)
#define PACKET_IS(lp, type)             (PACKET_TYPE(lp) == (type))
#define PACKET_HEADER(type, id)         ((PROTOCOL_VERSION << 24) | ((type) & 0x00ff0000) | ((id) & 0xffff))
#define PACKET_ID(lp)                   ((lp).header & 0xffff) // should only be set for sync/handshake packets
#define PACKET_VALUE_DELAY(delay, ping) ((((delay) >= 0x100 ? 0xff : delay) & 0xff) | ((((ping) >= 0x1000000 ? 0xffffff : ping) & 0xffffff) << 8))
#define PACKET_AGAIN(from, to)          (((from) & 0xffff) | (((to) & 0xffff) << 16))
#define PACKET_FROM(lp)                 ((lp).value & 0xffff)
#define PACKET_TO(lp)                   (((lp).value >> 16) & 0xffff)
#define PACKET_DELAY(lp)                ((lp).value & 0xff)
#define PACKET_PING(lp)                 (((lp).value & 0xffffff00) >> 8)

typedef enum
{
	EMPTY = 0,
	PROCESSED,     // this packet has been sent to the game as input or is yet to be received
	RECEIVED,      // this packet has been received
	SKIPPED,       // a packet with higher ID has been received, but not this one
	SENT,          // this local packet has been sent
} packet_state;

#ifdef _MSC_VER
#pragma pack(push, 1)
#define PACKED_STRUCT
#endif

typedef struct _luna_packet
{
	unsigned int header; // h & 0xff000000 == PACKET_VERSION << 24 ; h && 0x00ff0000 == packet type ; h & 0x0000ffff == packet id
	unsigned int low_id;
	unsigned int high_id;
	unsigned int value;
	WORD backup[BACKUP_VALUES];
} PACKED_STRUCT luna_packet;

typedef struct _history_entry
{
	packet_state state;
	DWORD value;
} PACKED_STRUCT history_entry;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

// history for local and remote inputs
class InputHistory
{
	private:
		int d;
		int size;
		int read_pos;
		int write_pos;
		history_entry *hist;
		HANDLE mutex;
		int set (int pos, int v);

	public:
		InputHistory ();
		~InputHistory ();
		void init (int d, packet_state init);
		history_entry get (int *p);
		void fake ();
		luna_packet put (int v);
		int put (luna_packet lp);
		luna_packet resend ();
		luna_packet *resend (int low, int high, int *n); // free buffer after use
		luna_packet request ();
		int get_refill ();
		void ensure (int position);
};


// spectator types and defines
#define SPECTATOR_INITIAL 64*3
#define JUMBO_SLEEP 250        // time to sleep between sending jumbo packets
#define JUMBO_VALUES 100       // 200 = 1sec, ponder about this value

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

typedef struct _luna_jumbo_packet
{
	unsigned int header; // h & 0xff000000 == PACKET_VERSION << 24 ; h && 0x00ff0000 == packet type ; h & 0x0000ffff == packet id
	unsigned int position;
	unsigned int values[JUMBO_VALUES];
} PACKED_STRUCT luna_jumbo_packet; // used for spectators

#ifdef _MSC_VER
#pragma pack(pop)
#endif

typedef struct _spectator
{
	unsigned int position;
	unsigned long peer;
	int phase;
} spectator;

typedef std::map<const unsigned long, spectator> spec_map;


// stuff
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#define MAX(a,b) ((a) < (b) ? (b) : (a))

#endif
