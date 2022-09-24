#ifdef _MSC_VER
#pragma warning (disable : 4786)
#endif

#include <direct.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <iostream>
#include "LunaPort.h"
#include "Code.h"
#include "ConManager.h"
using namespace std;

// don't like these globals and gotos? tough

FILE *logfile;                 // debug log file
HANDLE this_proc;              // LunaPort handle
HANDLE proc;                   // Vanguard Princess handle
HANDLE sem_recvd_input;        // new remote input has been received
HANDLE mutex_print;            // ensure prints are ordered/atomic
HANDLE mutex_history;          // lock changes to game	history
HANDLE event_running;          // game has started
HANDLE event_waiting;          // game is waiting
SOCKET sock;                   // socket used for communication
unsigned long remote_player;   // remote player
int local_p;                   // 0 = local p1, 1 = local p2
int delay = 0;                 // delay of input requests
InputHistory local_history;    // buffer for local input
InputHistory remote_history;   // buffer for remote input
char dir_prefix[_MAX_PATH];             // path prefix
unsigned int *game_history;             // history of the complete game session
unsigned int game_history_pos;          // current position in game history
unsigned int spec_pos;                  // maximum received position of this spectator
spec_map spectators;                    // map of spectators
int allow_spectators;                   // 0 = do not allow spectators
unsigned int port;                      // network port
unsigned int max_stages;                // number of stages
int rnd_seed;                           // seed of current game
FILE *write_replay;                     // write replay here
bool recording_ended;                   // set after faking a packet, this ensures it is not written to replay file
int ask_spectate;
int display_framerate;
int display_inputrate;
int display_names;
char game_exe[_MAX_PATH];
char own_name[NET_STRING_BUFFER], p1_name[NET_STRING_BUFFER], p2_name[NET_STRING_BUFFER];
ConManager conmanager;

void l () { /*WaitForSingleObject(mutex_print, INFINITE);*/ } // l() to lock print mutex
void u () { /*ReleaseMutex(mutex_print);*/ } // u() to unlock print mutex
char *ip2str (unsigned long ip)
{
	SOCKADDR_IN sa;
	sa.sin_addr.s_addr = ip;
	return inet_ntoa(sa.sin_addr);
}

// implementation of InputHistory (split into separate file later)
InputHistory::InputHistory ()
{
	this->size = 0;
	this->hist = NULL;
	this->d = 0;
	this->read_pos = 0;
	this->write_pos = 0;
	this->mutex = NULL;
}
void InputHistory::init (int d, packet_state init)
{
	SECURITY_ATTRIBUTES sa;
	ZeroMemory(&sa, sizeof(sa));
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;
	sa.nLength = sizeof(sa);

	if (mutex != NULL)
	{
		WaitForSingleObject(mutex, 5);
		CloseHandle(mutex);
	}

	this->size = HISTORY_CHUNK_SIZE;
	if (hist != NULL)
		free(hist);
	this->hist = (history_entry *)calloc(size, sizeof(history_entry));
	this->d = d;
	this->read_pos = 0;
	this->write_pos = d;
	this->mutex = CreateMutex(&sa, FALSE, NULL);

	WaitForSingleObject(mutex, INFINITE);
	for (int i = 0; i < d; i++)
	{
		hist[i].state = init;
		hist[i].value = 0;
	}
	ReleaseMutex(mutex);
}
InputHistory::~InputHistory ()
{
	free(hist);
	CloseHandle(mutex);
}
void InputHistory::fake ()
{
	history_entry he;
	WaitForSingleObject(mutex, INFINITE);
	ensure(read_pos + 1);
	for (int i = 0; i < 2; i++)
	{
		he = hist[read_pos+i];
		if (he.state != RECEIVED)
		{
			recording_ended = true;
			he.state = RECEIVED;
			he.value = 0;
			hist[read_pos+i] = he;
		}
	}
	ReleaseMutex(mutex);
}
history_entry InputHistory::get (int *p)
{
	history_entry he;
	WaitForSingleObject(mutex, INFINITE);
	if (p != NULL)
		*p = read_pos;
	he = hist[read_pos];
	if (he.state != SKIPPED)
		read_pos++;
	ReleaseMutex(mutex);
	return he;
}
int InputHistory::get_refill ()
{
	int diff;
	WaitForSingleObject(mutex, INFINITE);
	diff = d - (write_pos - read_pos);
	ReleaseMutex(mutex);
	return diff;
}
void InputHistory::ensure (int position)
{
	if (position > size - 1)
	{
		size += HISTORY_CHUNK_SIZE;
		hist = (history_entry *)realloc(hist, size * sizeof(history_entry));
		ZeroMemory(((char *)hist) + size - HISTORY_CHUNK_SIZE * sizeof(history_entry), HISTORY_CHUNK_SIZE * sizeof(history_entry));
	}
}
luna_packet InputHistory::put (int v)
{
	luna_packet lp;
	int min, max, j;
	ZeroMemory(&lp, sizeof(lp));
	WaitForSingleObject(mutex, INFINITE);
	min = MAX(0, write_pos - BACKUP_VALUES);
	max = write_pos;
	lp.header = PACKET_HEADER(PACKET_TYPE_INPUT, 0);
	lp.low_id = min;
	lp.high_id = max;
	ensure(write_pos);
	hist[write_pos].state = SENT;
	hist[write_pos++].value = v;
	lp.value = hist[min].value;
	j = 0;
	for (min++; min <= max; min++)
		lp.backup[j++] = hist[min].value;
	ReleaseMutex(mutex);
	return lp;
}
luna_packet InputHistory::resend ()
{
	luna_packet lp;
	int min, max, j;
	ZeroMemory(&lp, sizeof(lp));
	WaitForSingleObject(mutex, INFINITE);
	min = read_pos;
	max = MIN(MIN(read_pos + d, read_pos + BACKUP_VALUES), write_pos - 1);
	ensure(max);
	lp.header = PACKET_HEADER(PACKET_TYPE_INPUT, 0);
	lp.low_id = min;
	lp.high_id = max;
	lp.value = hist[min].value;
	j = 0;
	for (min++; min <= max; min++)
		lp.backup[j++] = hist[min].value;
	ReleaseMutex(mutex);
	return lp;
}
luna_packet *InputHistory::resend (int low, int high, int *n)
{
	luna_packet *packets;
	int j;
	*n = 0;
	if (high >= write_pos)
	{
		if (DEBUG) { l(); fprintf(logfile, "Resend: Clamping high from %08x to %08x.\n", high, write_pos); u(); }
		high = write_pos - 1;
	}
	if (high - low > 2*d || low > high)
	{
		if (DEBUG) { l(); fprintf(logfile, "Resend: Exiting early. High: %08x Low: %08x Delay: %08x\n", high, low, d); u(); }
		return NULL;
	}
	*n = (int)ceil((double)(high - low + 1)/(double)(1 + BACKUP_VALUES));
	if (DEBUG) { l(); fprintf(logfile, "Resend preparing %d packets.\n", *n); u(); }
	packets = (luna_packet *)calloc(*n, sizeof(luna_packet));
	WaitForSingleObject(mutex, INFINITE);
	ensure(high);
	for (int i = 0; i < *n; i++)
	{
		packets[i].header = PACKET_HEADER(PACKET_TYPE_INPUT, 0);
		packets[i].low_id = low;
		packets[i].high_id = low;
		packets[i].value = hist[low].value;
		j = 0;
		for (low++; low <= high; low++)
		{
			packets[i].high_id++;
			packets[i].backup[j++] = hist[low].value;
		}
		if (DEBUG) { l(); fprintf(logfile, "Resend prepared packet with range %08x to %08x.\n", packets[i].low_id, packets[i].high_id); u(); }
	}
	ReleaseMutex(mutex);
	return packets;
}
luna_packet InputHistory::request ()
{
	luna_packet lp;
	ZeroMemory(&lp, sizeof(lp));
	WaitForSingleObject(mutex, INFINITE);
	lp.header = PACKET_HEADER(PACKET_TYPE_OK, 0); // oops?
	if (hist[read_pos].state != SKIPPED)
	{
		ReleaseMutex(mutex);
		return lp;
	}

	lp.header = PACKET_HEADER(PACKET_TYPE_AGAIN, 0);
	lp.low_id = read_pos;
	for (lp.high_id = read_pos; hist[lp.high_id + 1].state == SKIPPED && lp.high_id + 1 <= read_pos + 2*d; lp.high_id++);

	ReleaseMutex(mutex);
	return lp;
}
int InputHistory::put (luna_packet lp)
{
	int gain = 0, j, min;
	WaitForSingleObject(mutex, INFINITE);
	if (DEBUG) { l(); fprintf(logfile, "Putting packet.\n"); u(); }
	if (read_pos + d * 2 < lp.high_id || lp.high_id - lp.low_id > BACKUP_VALUES || lp.low_id > lp.high_id) // do not accept packets with broken ranges or from too far in the future
	{
		if (DEBUG) { l(); fprintf(logfile, "Ignoring packet, out of range: %08x@%08x-%08x\n", read_pos + d, lp.low_id, lp.high_id); u(); }
		ReleaseMutex(mutex);
		return gain;
	}
	ensure(lp.high_id);
	min = lp.low_id;
	gain += set(min, lp.value);
	j = 0;
	for (min++; min <= lp.high_id; min++)
		gain += set(min, lp.backup[j++]);
	for (min = write_pos; min < lp.low_id; min++)
		if (hist[min].state == EMPTY)
		{
			hist[min].state = SKIPPED;
			gain++;
		}
	write_pos = lp.high_id;
	ReleaseMutex(mutex);
	return gain;
}
int InputHistory::set (int pos, int v)
{
	int gain;
	if (pos < read_pos)
		return 0;
	if (read_pos + d < pos)
		return 0;
	hist[pos].value = v;
	if (hist[pos].state == EMPTY)
		gain = 1;
	else
		gain = 0;
	hist[pos].state = RECEIVED;
	return gain;
}

void update_history (unsigned int v)
{
	WaitForSingleObject(mutex_history, INFINITE);
	if (game_history_pos % HISTORY_CHUNK_SIZE == 0)
		game_history = (unsigned int *)realloc(game_history, 4 * HISTORY_CHUNK_SIZE * (1 + game_history_pos / HISTORY_CHUNK_SIZE));
	game_history[game_history_pos++] = v;
	ReleaseMutex(mutex_history);
	if (write_replay != NULL && !recording_ended)
		fwrite(&v, 1, 4, write_replay);
}

void send_local_input (int v)
{
	luna_packet lp;

	if (DEBUG) { l(); fprintf(logfile, "Local handler putting current v.\n"); u(); }
	lp = local_history.put(v);

	if (DEBUG)
	{
		l(); fprintf(logfile, "LISend: %08x %08x %02x %02x %08x ", lp.low_id, lp.high_id, PACKET_TYPE_LOW(lp), PACKET_VERSION(lp), lp.value);
		for (int i = 0; i < BACKUP_VALUES; i++)
			fprintf(logfile, "%04x ", lp.backup[i]);
		fprintf(logfile, "\n");
		u();
	}

	conmanager.send(&lp, remote_player);
}

int local_handler (int v)
{
	if (DEBUG) { l(); fprintf(logfile, "Local handler sending %08x.\n", v); u(); }
	send_local_input(v);

	if (DEBUG) { l(); fprintf(logfile, "Local handler retrieving current.\n"); u(); }
	v = local_history.get(NULL).value;
	if (DEBUG) { l(); fprintf(logfile, "Local input at: %08x.\n", v); u(); }

	return v;
}

int remote_handler ()
{
	history_entry he;
	luna_packet lp;
	int pos;

	if (DEBUG) { l(); fprintf(logfile, "Remote handler, waiting recvd.\n"); u(); }
	if (WaitForSingleObject(sem_recvd_input, SPIKE_INTERVAL) == WAIT_TIMEOUT)
	{
		int rebuffer = 0;
		if (DEBUG) { l(); fprintf(logfile, "Refilling delay buffer.\n"); u(); }
		rebuffer = local_history.get_refill();
		for (int i = 0; i < rebuffer; i++)
			send_local_input(0);
		WaitForSingleObject(sem_recvd_input, INFINITE);
	}
	if (DEBUG) { l(); fprintf(logfile, "Remote handler, done waiting recvd.\n"); u(); }

	do
	{
		he = remote_history.get(&pos);
		switch (he.state)
		{
		case SKIPPED:
			l();
			printf("Lost input %08x. Waiting for resend.\n", pos);
			if (DEBUG) { fprintf(logfile, "Input packet %08x lost.\n", pos); }
			u();
			lp = remote_history.request();
			if (PACKET_IS(lp, PACKET_TYPE_OK))
				break;
			if (DEBUG) { l(); fprintf(logfile, "Asking for resend (%08x).\n", pos); u();}
			if (DEBUG)
			{
				l(); fprintf(logfile, "ReqSend: %08x %08x %02x %02x %08x ", lp.low_id, lp.high_id, PACKET_TYPE_LOW(lp), PACKET_VERSION(lp), lp.value);
				for (int i = 0; i < BACKUP_VALUES; i++)
					fprintf(logfile, "%04x ", lp.backup[i]);
				fprintf(logfile, "\n");
				u();
			}
			conmanager.send(&lp, remote_player);
			Sleep(5);
			break;
		case RECEIVED:
			break;
		default:
			l(); printf("The impossible happened. State of packet %08x is %02x. Cloned previous.\n", pos, he.state);
			if (DEBUG) { fprintf(logfile, "The impossible happened. State of packet %08x is %02x. Cloned previous.\n", pos, he.state); } u();
			break;
		}
	} while (he.state == SKIPPED);
	if (DEBUG) { l(); fprintf(logfile, "Got remote input %08x at %08x.\n", he.value, pos); u(); }

	return he.value;
}

void write_net_string (char *str, FILE *hnd)
{
	int i = 0;
	while (i < NET_STRING_BUFFER - 1 && str[i])
	{
		fwrite(&(str[i]), 1, 1, hnd);
		i++;
	}
	i = 0;
	fwrite(&i, 1, 1, hnd);
}

void read_net_string (char *str, FILE *hnd)
{
	int i = 0;
	char r = '\1';
	while (i < NET_STRING_BUFFER - 1 && r)
	{
		fread(&r, 1, 1, hnd);
		str[i] = r;
		i++;
	}
	str[i] = 0;
}

FILE *create_replay_file ()
{
	FILE *hnd;
	char filename[_MAX_PATH], path[_MAX_PATH]; // enough
	time_t ltime;
	struct tm *tm;

	ZeroMemory(filename, sizeof(filename));
	_tzset();
	time(&ltime);
	tm = localtime(&ltime);
	strftime(filename, 200, "Replays\\%Y-%m-%d-%H-%M-%S.rpy", tm);
	strcpy(path, dir_prefix);
	strcat(path, "\\");
	strcat(path, filename);
	printf("Saving replay: %s\n", filename);
	hnd = fopen(path, "wb");
	setvbuf(hnd, NULL, _IONBF, 0);
	fwrite(REPLAY_HEADER_V2, 4, 1, hnd);
	write_net_string(p1_name, hnd);
	write_net_string(p2_name, hnd);
	write_replay = hnd;
	return hnd;
}

FILE *open_replay_file (char *filename, int *seed)
{
	FILE *hnd;
	char buf[4];
	char fin_filename[_MAX_PATH];

	ZeroMemory(fin_filename, _MAX_PATH);
	if (filename[1] == ':')
		strcpy(fin_filename, filename);
	else
	{
		strcpy(fin_filename, dir_prefix);
		strcat(fin_filename, "\\Replays\\");
		strcat(fin_filename, filename);
	}
	printf("Playing replay: %s\n", fin_filename);
	if ((hnd = fopen(fin_filename, "rb")) == NULL)
	{
		printf("Cannot open replay file.\n");
		return NULL;
	}
	fread(buf, 4, 1, hnd);
	if (!(!strncmp(buf, REPLAY_HEADER, 4) || !strncmp(buf, REPLAY_HEADER_V2, 4)))
	{
		printf("Invalid replay header.\n");
		return NULL;
	}
	if (!strncmp(buf, REPLAY_HEADER_V2, 4))
	{
		read_net_string(p1_name, hnd);
		read_net_string(p2_name, hnd);
	}
	fread(seed, 4, 1, hnd);
	if (DEBUG) { l(); fprintf(logfile, "Read replay seed %08x.\n", *seed); u(); }
	return hnd;
}

void replay_input_handler (void *address, HANDLE proc_thread, FILE *replay, int record_replay, int network, int single_player, int spectator)
{
	CONTEXT c;

	ZeroMemory(&c, sizeof(c));
	c.ContextFlags = CONTEXT_INTEGER;
	GetThreadContext(proc_thread, &c);

	if (spectator)
	{
		if (DEBUG) { l(); fprintf(logfile, "Waiting for spec recvd.\n"); u(); }
		WaitForSingleObject(sem_recvd_input, INFINITE);
		if (DEBUG) { l(); fprintf(logfile, "Done waiting recvd. Waiting for spec history.\n"); u(); }
		WaitForSingleObject(mutex_history, INFINITE);
		c.Eax = game_history[spec_pos++];
		if (DEBUG) { l(); fprintf(logfile, "Got spec input %08x at %08x.\n", c.Eax, spec_pos-1); u(); }
		ReleaseMutex(mutex_history);
		SetThreadContext(proc_thread, &c);
		if (DEBUG) { l(); fprintf(logfile, "Set spec input.\n"); u(); }
		FlushInstructionCache(proc, NULL, 0); // shouldn't hurt
	}
	else
	{
		if (network)
		{
			if (local_p == single_player)
				c.Eax = local_handler(c.Eax);
			else
				c.Eax = remote_handler();
		}
		update_history(c.Eax);
	}

	if (replay != NULL)
	{
		if (record_replay == -1) // playback
		{
			if (!feof(replay) && fread(&(c.Eax), 1, 4, replay) == 4)
			{
				if (DEBUG) { l(); fprintf(logfile, "Read input %08x at %08x.\n", c.Eax, address); u(); }
				SetThreadContext(proc_thread, &c);
				FlushInstructionCache(proc, NULL, 0); // shouldn't hurt
				return;
			}

			fclose(replay);
			replay = NULL;
			l(); printf("Replay ended.\n"); u();
			remote_history.fake();
			ReleaseSemaphore(sem_recvd_input, 2, NULL);
			TerminateProcess(proc, 0);
		}
	}
}

void set_caption (void *p)
{
	HWND game_window = FindWindow("KGT2KGAME", NULL);
	SetWindowText(game_window, (char *)p);
}

void send_foreground (void *p)
{
	HWND game_window = FindWindow("KGT2KGAME", NULL);
	SetWindowPos(game_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE);
	SetWindowPos(game_window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE);
}

int run_game (int seed, int network, int record_replay, char *filename, int spectator)
{
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DEBUG_EVENT de;
	CONTEXT c;
	DWORD state;
	HANDLE proc_thread;
	DWORD proc_thread_id;
	FILE *replay = NULL;
	int dlls = 0;
	int remote_p = local_p ? 0 : 1;
	int single_player = 0;
	char path[_MAX_PATH*2];
	char *debugstring;
	unsigned char nop = 0x90;
	void *address;
	DWORD frames = 0, now, last_sec = 0, inputs = 0;
	char title_base[1024], window_title[1280];

	rnd_seed = seed;
	SetEvent(event_running);

	if (record_replay == 1)
		replay = create_replay_file();
	else if (record_replay == -1)
		replay = open_replay_file(filename, &seed);

	if (record_replay && replay == NULL)
	{
		printf("Cannot open replay file. Aborting.\n");
		return 0;
	}

	if (DEBUG){ l(); fprintf(logfile, "Seed: %08x\n", seed); u(); }

	ZeroMemory(&title_base, sizeof(title_base));
	ZeroMemory(&window_title, sizeof(window_title));
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	strcpy(path, dir_prefix);
	strcat(path, "\\");
	strcat(path, game_exe);
	l(); printf("Running: %s\n", path); u();
	if (!CreateProcess(path, NULL, NULL, NULL, FALSE, DEBUG_PROCESS, NULL, NULL, &si, &pi))
	{
		l(); printf("Couldn't find game exe. Trying alternative.\n"); u();
		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		strcpy(path, dir_prefix);
		strcat(path, "\\");
		strcat(path, ALTGAME);
		l(); printf("Running: %s\n", path); u();
		if (!CreateProcess(path, NULL, NULL, NULL, FALSE, DEBUG_PROCESS, NULL, NULL, &si, &pi))
		{
			l(); printf("Couldn't find alternative game exe. Giving up.\n"); u();
			return 0;
		}
	}
	proc = OpenProcess(PROCESS_ALL_ACCESS, false, pi.dwProcessId);

	if (!spectator)
		update_history(seed); // first entry of the game history is the seed

	while (WaitForDebugEvent(&de, INFINITE))
	{
		state = DBG_CONTINUE;
		switch (de.dwDebugEventCode)
		{
		case EXCEPTION_DEBUG_EVENT:
			if (DEBUG) { l(); fprintf(logfile, "EXCEPTION_DEBUG_EVENT: %08x@%08x\n", de.u.Exception.ExceptionRecord.ExceptionCode, de.u.Exception.ExceptionRecord.ExceptionAddress); u(); }
			if (de.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
			{
				l(); printf("Access Violation %08x at %08x: %s@%08x\n", de.u.Exception.ExceptionRecord.ExceptionCode, de.u.Exception.ExceptionRecord.ExceptionAddress, de.u.Exception.ExceptionRecord.ExceptionInformation[0] ? "read" : "write", de.u.Exception.ExceptionRecord.ExceptionInformation[1]);
				if (DEBUG) fprintf(logfile, "Access Violation %08x at %08x: %s@%08x\n", de.u.Exception.ExceptionRecord.ExceptionCode, de.u.Exception.ExceptionRecord.ExceptionAddress, de.u.Exception.ExceptionRecord.ExceptionInformation[0] ? "read" : "write", de.u.Exception.ExceptionRecord.ExceptionInformation[1]); u();
				state = DBG_EXCEPTION_NOT_HANDLED;
				Sleep(3000);
			}
			if (de.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT)
			{
				if (de.dwThreadId != proc_thread_id)
					break;
				address = de.u.Exception.ExceptionRecord.ExceptionAddress;

				switch ((unsigned int)address)
				{
				case CONTROL_CHANGE:
					ReadProcessMemory(proc, (void *)P1_KBD_CONTROLS, kbd_control_buffer, sizeof(kbd_control_buffer), NULL); // get p1 keyboard controls
					WriteProcessMemory(proc, (void *)P2_KBD_CONTROLS, kbd_control_buffer, sizeof(kbd_control_buffer), NULL); // set p2 keyboard controls as those of p1
					ReadProcessMemory(proc, (void *)P1_JOY_CONTROLS, joy_control_buffer, sizeof(joy_control_buffer), NULL); // get p1 joystick controls
					WriteProcessMemory(proc, (void *)P2_JOY_CONTROLS, joy_control_buffer, sizeof(joy_control_buffer), NULL); // set p2 joystick controls as those of p1
					WriteProcessMemory(proc, (void *)STICK_SELECTION, stick_selection, sizeof(stick_selection), NULL); // always read input from first joystick

					// disable control config writeback
					WriteProcessMemory(proc, (void *)KBD_WRITEBACK, kbd_writeback, sizeof(kbd_writeback), NULL); // don't write changes to keyboard control config
					WriteProcessMemory(proc, (void *)JOY_WRITEBACK, joy_writeback, sizeof(joy_writeback), NULL); // don't write changes to joystick control config

					// disable control change installation
					WriteProcessMemory(proc, (void *)CONTROL_CHANGE, &nop, 1, NULL);
					break;


				case LOCAL_P_BREAK:
					inputs++;
					c.ContextFlags = CONTEXT_INTEGER;
					GetThreadContext(proc_thread, &c);
					if (DEBUG) { l(); fprintf(logfile, "Got LPB (%08x).\n", c.Eax); u(); }
					c.Eax = local_handler(c.Eax);
					update_history(c.Eax);
					SetThreadContext(proc_thread, &c);

					FlushInstructionCache(proc, NULL, 0); // shouldn't hurt
					break;


				case REMOTE_P_BREAK:
					inputs++;
					if (DEBUG) { l(); fprintf(logfile, "Got RPB.\n"); u(); }
					c.ContextFlags = CONTEXT_INTEGER;
					GetThreadContext(proc_thread, &c);
					c.Eax = remote_handler();
					update_history(c.Eax);
					SetThreadContext(proc_thread, &c);
					if (DEBUG) { l(); fprintf(logfile, "Put RPB (%08x).\n", c.Eax); u(); }

					FlushInstructionCache(proc, NULL, 0); // shouldn't hurt
					break;


				case REPLAY_P1_BREAK: // we can use the same handler in both cases, since we do the same thing
				case REPLAY_P2_BREAK:
					inputs++;
					if (DEBUG) { l(); fprintf(logfile, "Entering input handler.\n"); u(); }
					if (record_replay || spectator)
						replay_input_handler(address, proc_thread, replay, record_replay, 0, 0, spectator);
					if (DEBUG) { l(); fprintf(logfile, "Leaving input handler.\n"); u(); }
					break;


				case SINGLE_BREAK_CONTROL:
					c.ContextFlags = CONTEXT_INTEGER;
					GetThreadContext(proc_thread, &c);
					single_player = c.Ecx & 1;
					break;


				case SINGLE_BREAK_INPUT:
					inputs += 2;
					if (record_replay || spectator)
					{
						replay_input_handler(address, proc_thread, replay, record_replay, network, single_player, spectator);
						break;
					}

					c.ContextFlags = CONTEXT_INTEGER;
					GetThreadContext(proc_thread, &c);

					// while in single player mode, we are basically just streaming one-sidedly
					// the keep_alive thread should keep us going in the other direction
					// this should work, in theory
					if (local_p == single_player)
						c.Eax = local_handler(c.Eax);
					else
						c.Eax = remote_handler();

					update_history(c.Eax);
					SetThreadContext(proc_thread, &c);
					FlushInstructionCache(proc, NULL, 0); // shouldn't hurt
					break;


				case TITLE_BREAK:
					if (DEBUG) { l(); fprintf(logfile, "Entering title handler.\n"); u(); }
					c.ContextFlags = CONTEXT_FULL;
					GetThreadContext(proc_thread, &c);
					c.Eip--;
					SetThreadContext(proc_thread, &c);
					c.EFlags |= 0x0100; // single step for reinstallation of breakpoint
					ZeroMemory(title_base, sizeof(title_base));
					ReadProcessMemory(proc, (void *)c.Edx, title_base, sizeof(title_base) / 2, NULL); // read into title buffer, actually read too much, but never mind
					title_base[sizeof(title_base)-1] = 0;
					if (display_names && strlen(p1_name) && strlen(p2_name))
					{
						strcat(title_base, " - ");
						strcat(title_base, p1_name);
						strcat(title_base, " vs ");
						strcat(title_base, p2_name);
						strcpy(window_title, title_base);
						CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)set_caption, (void *)window_title, 0, NULL);
					}
					WriteProcessMemory(proc, (void *)TITLE_BREAK, title_break_bak, sizeof(title_break_bak), NULL); // reset old code
					FlushInstructionCache(proc, NULL, 0); // shouldn't hurt
					if (DEBUG) { l(); fprintf(logfile, "Leaving title handler.\n"); u(); }
					break;


				case FRAME_BREAK:
					frames++;
					if (DEBUG) { l(); fprintf(logfile, "Entering frame handler.\n"); u(); }
					if (!title_base[0])
						break;
					if (DEBUG) { l(); fprintf(logfile, "For real.\n"); u(); }
					now = GetTickCount();
					if (now - last_sec >= 1000)
					{
						if (DEBUG) { l(); fprintf(logfile, "Second started.\n"); u(); }
						strcpy(window_title, title_base);
						if (display_framerate)
						{
							strcat(window_title, " - FPS: ");
							sprintf(window_title + strlen(window_title), "%d", frames);
						}
						if (display_inputrate)
						{
							strcat(window_title, " - Inputs: ");
							sprintf(window_title + strlen(window_title), "%d", inputs / 2); // counted inputs for both players
						}
						if (display_framerate || display_inputrate)
							CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)set_caption, (void *)window_title, 0, NULL);
						frames = 0;
						inputs = 0;
						last_sec = now;
						if (DEBUG) { l(); fprintf(logfile, "Done.\n"); u(); }
					}
					if (DEBUG) { l(); fprintf(logfile, "Leaving frame handler.\n"); u(); }
					break;
				}
			}
			else if (de.u.Exception.ExceptionRecord.ExceptionCode == STATUS_SINGLE_STEP)
			{
				if (DEBUG) { l(); fprintf(logfile, "Entering single step handler.\n"); u(); }
				WriteProcessMemory(proc, (void *)TITLE_BREAK, title_break, sizeof(title_break), NULL); // reset breakpoint
				FlushInstructionCache(proc, NULL, 0); // shouldn't hurt
				if (DEBUG) { l(); fprintf(logfile, "Leaving single step handler.\n"); u(); }
			}
			break;


		case CREATE_THREAD_DEBUG_EVENT:
			if (DEBUG) { l(); fprintf(logfile, "CREATE_THREAD_DEBUG_EVENT\n"); u(); }
			break;
		case CREATE_PROCESS_DEBUG_EVENT:
			proc_thread = de.u.CreateProcessInfo.hThread;
			proc_thread_id = de.dwThreadId;
			if (DEBUG) { l(); fprintf(logfile, "CREATE_PROCESS_DEBUG_EVENT\n"); u(); }
			break;
		case EXIT_THREAD_DEBUG_EVENT:
			if (DEBUG) { l(); fprintf(logfile, "EXIT_THREAD_DEBUG_EVENT\n"); u(); }
			break;
		case EXIT_PROCESS_DEBUG_EVENT:
			if (DEBUG) { l(); fprintf(logfile, "EXIT_PROCESS_DEBUG_EVENT\n"); u(); }
			goto DONE;
			break;


		case LOAD_DLL_DEBUG_EVENT:
			dlls++;
			if (DEBUG) { l(); fprintf(logfile, "LOAD_DLL_DEBUG_EVENT(%d)\n", dlls); u(); }
			if (dlls == 17) // all we need is loaded into mem now
			{
				WriteProcessMemory(proc, (void *)DEBUG_BREAKPOINT, "\x90", 1, NULL); // disable ntdll.DbgBreakPoint with nop
				WriteProcessMemory(proc, (void *)RANDOM_SEED, &seed, 4, NULL); // initialize seed (they never even call srand)

				WriteProcessMemory(proc, (void *)MAX_STAGES_OFFSET, &max_stages, 4, NULL); // set stage selection code
				WriteProcessMemory(proc, (void *)STAGE_SELECT, stage_select, sizeof(stage_select), NULL); // set stage selection code
				WriteProcessMemory(proc, (void *)STAGE_SELECT_FUNC, stage_select_func, sizeof(stage_select_func), NULL); // set stage selection code

				WriteProcessMemory(proc, (void *)LOCAL_P_FUNC, local_p_func, sizeof(local_p_func), NULL); // local player input grabber
				WriteProcessMemory(proc, (void *)LOCAL_P_JMPBACK, local_p_jmpback[local_p], sizeof(local_p_jmpback[local_p]), NULL); // local player input grabber jump back

				WriteProcessMemory(proc, (void *)REMOTE_P_FUNC, remote_p_func, sizeof(remote_p_func), NULL); // remote player input grabber
				WriteProcessMemory(proc, (void *)REMOTE_P_JMPBACK, remote_p_jmpback[remote_p], sizeof(remote_p_jmpback[remote_p]), NULL); // remote player input grabber jump back

				if (display_framerate || display_inputrate)
				{
					WriteProcessMemory(proc, (void *)FRAME_FUNC, frame_func, sizeof(frame_func), NULL); // frame rate counter hook
					WriteProcessMemory(proc, (void *)FRAME_JUMP, frame_jump, sizeof(frame_jump), NULL); // frame rate counter jump
					WriteProcessMemory(proc, (void *)TITLE_BREAK, title_break, sizeof(title_break), NULL); // install breakpoint where window title is set
				}

				if (network || record_replay || spectator)
				{
					WriteProcessMemory(proc, (void *)SINGLE_FUNC, single_func, sizeof(single_func), NULL); // single player input grabber
					WriteProcessMemory(proc, (void *)SINGLE_JUMP, single_jump, sizeof(single_jump), NULL); // single player input grabber jump
				}

				if (network)
				{
					WriteProcessMemory(proc, (void *)P1_JUMP, p1_jump[local_p], sizeof(p1_jump[local_p]), NULL); // hook up input for p1
					WriteProcessMemory(proc, (void *)P2_JUMP, p2_jump[remote_p], sizeof(p2_jump[remote_p]), NULL); // hook up input for p2
				}
				else /*if (record_replay || spectator)*/ // local replay recording or replay playback
				{
					WriteProcessMemory(proc, (void *)REPLAY_HOOKS, replay_hooks, sizeof(replay_hooks), NULL); // hooks for replay recording
					WriteProcessMemory(proc, (void *)REPLAY_P1_JUMP, replay_p1_jump, sizeof(replay_p1_jump), NULL); // install for p1
					WriteProcessMemory(proc, (void *)REPLAY_P2_JUMP, replay_p2_jump, sizeof(replay_p2_jump), NULL); // install for p2
				}

				FlushInstructionCache(proc, NULL, 0); // shouldn't hurt

				// now we are basically done with debug stuff, all necessary code has been injected
			}
			if (dlls == 35)
			{
				CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)send_foreground, (void *)NULL, 0, NULL);
			}
			break;


		case UNLOAD_DLL_DEBUG_EVENT:
			if (DEBUG) { l(); fprintf(logfile, "UNLOAD_DLL_DEBUG_EVENT\n"); u(); }
			break;
		case OUTPUT_DEBUG_STRING_EVENT:
			if (DEBUG) {
				debugstring = (char*)malloc(de.u.DebugString.nDebugStringLength);
				ReadProcessMemory(proc, de.u.DebugString.lpDebugStringData, debugstring, de.u.DebugString.nDebugStringLength, NULL);
				l(); fprintf(logfile, "OUTPUT_DEBUG_STRING_EVENT(%s)\n", debugstring); u();
				free(debugstring);
			}
			break;
		case RIP_EVENT:
			if (DEBUG) { l(); fprintf(logfile, "RIP_EVENT\n"); u(); }
			break;
		}

		ContinueDebugEvent(de.dwProcessId, de.dwThreadId, state);
	}

DONE:
	if (replay != NULL)
	{
		fclose(replay);
		replay = NULL;
	}

	return 0;
}

int create_sems ()
{
	SECURITY_ATTRIBUTES sa;
	ZeroMemory(&sa, sizeof(sa));
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;
	sa.nLength = sizeof(sa);

	sem_recvd_input = CreateSemaphore(&sa, 0, LONG_MAX, NULL);
	mutex_history = CreateMutex(&sa, FALSE, NULL);

	return 0;
}

int close_sems ()
{
	CloseHandle(sem_recvd_input);
	CloseHandle(mutex_history);

	return 0;
}

void set_delay (int d)
{
	delay = d;

	// create/initialize semaphores
	if (delay) ReleaseSemaphore(sem_recvd_input, delay, NULL);

	// initialize input history tables
	local_history.init(d, SENT);
	remote_history.init(d, RECEIVED);
}

int get_ping (unsigned long peer)
{
	TIMECAPS tc;
	DWORD start, end;
	luna_packet lp;
	simple_luna_packet(&lp, PACKET_TYPE_PING, 0);

	timeGetDevCaps(&tc, sizeof(tc));
	timeBeginPeriod(tc.wPeriodMin);
	start = timeGetTime();

	if (!conmanager.send(&lp, peer))
		return -1;

	end = timeGetTime();
	timeEndPeriod(tc.wPeriodMin);
	return (end - start);
}

void serve_spectators (void *p)
{
	DWORD now;

	if (allow_spectators)
		for (;;)
		{
			Sleep(JUMBO_SLEEP);
			now = GetTickCount();

			WaitForSingleObject(mutex_history, INFINITE);
			for (spec_map::iterator i = spectators.begin(); i != spectators.end(); )
			{
				spectator s = i->second;
				if (s.phase == 0 && WaitForSingleObject(event_running, 0) == WAIT_TIMEOUT)
				{
					luna_packet lp;
					simple_luna_packet(&lp, PACKET_TYPE_ALIVE, 0);
					if (!conmanager.send_raw((void *)&lp, s.peer, sizeof(lp)))
					{
						l(); printf("Dropping spectator %s.\n", ip2str(s.peer));
						if (DEBUG) fprintf(logfile, "Dropping spectator %s.\n", ip2str(s.peer));
						u();
						conmanager.disconnect_nobye(s.peer);
						spectators.erase(i++);
						continue;
					}
				}
				else if (s.phase == 2 && s.position < game_history_pos - JUMBO_VALUES)
				{
					luna_jumbo_packet ljp;
					ljp.header = PACKET_HEADER(PACKET_TYPE_JUMBO, 0);
					ljp.position = s.position;
					i->second.position += JUMBO_VALUES;
					for (int j = 0; j < JUMBO_VALUES; j++)
						ljp.values[j] = game_history[s.position + j];
					if (!conmanager.send_jumbo(&ljp, s.peer))
					{
						l(); printf("Dropping spectator %s.\n", ip2str(s.peer));
						if (DEBUG) fprintf(logfile, "Dropping spectator %s.\n", ip2str(s.peer));
						u();
						conmanager.disconnect_nobye(s.peer);
						spectators.erase(i++);
						continue;
					}
				}
				i++;
			}
			ReleaseMutex(mutex_history);
		}
}

void keep_alive (unsigned long peer)
{
	luna_packet lp;
	for (;;)
	{
		lp = local_history.resend();
		if (DEBUG)
		{
			l(); fprintf(logfile, "KASend: %08x %08x %02x %02x %08x ", lp.low_id, lp.high_id, PACKET_TYPE_LOW(lp), PACKET_VERSION(lp), lp.value);
			for (int i = 0; i < BACKUP_VALUES; i++)
				fprintf(logfile, "%04x ", lp.backup[i]);
			fprintf(logfile, "\n");
			u();
		}
		conmanager.send(&lp, peer);
		Sleep(ALIVE_INTERVAL);
	}
}

void bye_spectators ()
{
	for (spec_map::iterator i = spectators.begin(); i != spectators.end(); i++)
	{
		spectator s = i->second;
		conmanager.disconnect(s.peer);
	}
}

void spec_keep_alive (unsigned long peer)
{
	luna_packet lp;
	for (;;)
	{
		if (WaitForSingleObject(event_running, 0) != WAIT_OBJECT_0)
		{
			simple_luna_packet(&lp, PACKET_TYPE_IGNOR, 0);
			conmanager.send_raw(&lp, peer, sizeof(luna_packet));
		}
		else
		{
			WaitForSingleObject(mutex_history, INFINITE);
			simple_luna_packet(&lp, PACKET_TYPE_SPECT, 0);
			lp.value = game_history_pos;
			conmanager.send_raw(&lp, peer, sizeof(luna_packet));
			ReleaseMutex(mutex_history);
		}
		Sleep(ALIVE_INTERVAL);
	}
}

void spec_receiver (AsyncPacket *packet)
{
	luna_jumbo_packet *ljp = (luna_jumbo_packet *)(packet->packet);
	unsigned long peer = packet->peer;
	long left = 0;

	switch (PACKET_TYPE(*ljp))
	{
	case PACKET_TYPE_TOUT:
		l();
		printf("Connection lost. No packets received for 10s.\n");
		u();
		if (proc != NULL)
		{
			remote_history.fake();
			ReleaseSemaphore(sem_recvd_input, 2, NULL);
			TerminateProcess(proc, 0);
		}
		break;
	case PACKET_TYPE_ERROR:
	case PACKET_TYPE_BYE:
		if (proc != NULL)
		{
			l();
			printf("Connection closed.\n");
			u();
			remote_history.fake();
			ReleaseSemaphore(sem_recvd_input, 2, NULL);
			TerminateProcess(proc, 0);
		}
		break;
	case PACKET_TYPE_JUMBO:
		WaitForSingleObject(mutex_history, INFINITE);
		if (game_history_pos != ljp->position)
		{
			ReleaseMutex(mutex_history);
			break;
		}
		ReleaseMutex(mutex_history);
		for (int i = 0; i < JUMBO_VALUES; i++)
			update_history(ljp->values[i]);
		ReleaseSemaphore(sem_recvd_input, JUMBO_VALUES, &left);
		if (DEBUG) { l(); fprintf(logfile, "Spec recvd semaphore now contains %u.\n", left); u(); }
		break;
	}
}

void peer_receiver (AsyncPacket *packet)
{
	unsigned long peer = packet->peer;
	luna_packet *lp = (luna_packet *)(packet->packet);
	luna_packet *resends;
	unsigned int old_recvd, i;
	int n;
	int gained;
	spectator s;

	switch (PACKET_TYPE(*lp))
	{
	case PACKET_TYPE_INPUT:
		if (peer != remote_player)
			break;

		gained = remote_history.put(*lp);

		if (gained)
		{
			if (DEBUG) { l(); fprintf(logfile, "Releasing semaphore recvd_input (%04x).\n", gained); u(); }
			ReleaseSemaphore(sem_recvd_input, gained, (long *)&old_recvd);
			if (DEBUG) { l(); fprintf(logfile, "Now recvd_input is %08x.\n", old_recvd); u(); }
		}
		else
			if (DEBUG) { l(); fprintf(logfile, "Semaphore recvd_input not released (%04x).\n", gained); u(); }
		break;
	case PACKET_TYPE_ERROR:
	case PACKET_TYPE_BYE:
		if (peer != remote_player)
			break;

		if (proc != NULL)
		{
			l();
			printf("Connection closed.\n");
			u();
			remote_history.fake();
			ReleaseSemaphore(sem_recvd_input, 2, NULL);
			TerminateProcess(proc, 0);
		}
		break;
	case PACKET_TYPE_AGAIN:
		if (peer != remote_player)
			break;

		if (DEBUG) { l(); fprintf(logfile, "Got resend request from %08x to %08x.\n", lp->low_id, lp->high_id); u(); }
		resends = local_history.resend(lp->low_id, lp->high_id, &n);
		for (i = 0; i < n; i++)
		{
			if (DEBUG)
			{
				l(); fprintf(logfile, "AgainSend: %08x %08x %02x %02x %08x ", resends[i].low_id, resends[i].high_id, PACKET_TYPE_LOW(resends[i]), PACKET_VERSION(resends[i]), resends[i].value);
				for (int i = 0; i < BACKUP_VALUES; i++)
					fprintf(logfile, "%04x ", resends[i].backup[i]);
				fprintf(logfile, "\n");
				u();
			}

			conmanager.send(&(resends[i]), peer);
		}
		if (resends != NULL)
			free(resends);
		break;
	case PACKET_TYPE_SPECT:
		if (!spectators.count(peer))
			break;

		WaitForSingleObject(mutex_history, INFINITE);
		s = spectators[peer];
		s.position = lp->value;
		spectators[peer] = s;
		ReleaseMutex(mutex_history);
		break;
	case PACKET_TYPE_TOUT:
		if (peer != remote_player && spectators.count(peer))
		{
			l(); printf("Dropping spectator %s.\n", ip2str(peer));
			if (DEBUG) fprintf(logfile, "Dropping spectator %s.\n", ip2str(peer));
			u();

			WaitForSingleObject(mutex_history, INFINITE);
			spectators.erase(peer);
			ReleaseMutex(mutex_history);
			if (DEBUG) { l(); fprintf(logfile, "Dropped spectator %s.\n", ip2str(peer)); u(); }
		}
		else if (peer == remote_player)
		{
			l();
			printf("Connection lost. No packets received for 10s.\n");
			u();
			if (proc != NULL)
			{
				remote_history.fake();
				ReleaseSemaphore(sem_recvd_input, 2, NULL);
				TerminateProcess(proc, 0);
			}
		}
		break;
	}
}

void clear_socket ()
{
	int r;
	char buf[1024];
	Sleep(500);
	do
	{
		r = recv(sock, buf, 1024, 0);
	} while (r != 0 && r != SOCKET_ERROR);
}

bool receive_string (char *buf, unsigned long peer)
{
	luna_packet lp;
	char *old_buf = buf;
	
	ZeroMemory(buf, NET_STRING_BUFFER);

	do
	{
		if (!conmanager.receive(&lp, peer))
			return false;

		if (PACKET_IS(lp, PACKET_TYPE_TEXT) && buf - old_buf <= NET_STRING_LENGTH - NET_STRING_PERPACKET)
		{
			memcpy(buf, &(lp.value), NET_STRING_PERPACKET);
			buf += MIN(lp.low_id, NET_STRING_PERPACKET);
		}
		else if (!PACKET_IS(lp, PACKET_TYPE_DONE))
			return false;
	} while (buf - old_buf <= NET_STRING_LENGTH && !PACKET_IS(lp, PACKET_TYPE_DONE));

	if (!PACKET_IS(lp, PACKET_TYPE_DONE))
		return false;

	return true;
}

bool send_string (char *buf, unsigned long peer)
{
	luna_packet lp;
	char *old_buf = buf;
	int len = MAX(0, MIN(strlen(buf), NET_STRING_LENGTH));
	int packets = (int)ceil(((double)len) / ((double)NET_STRING_PERPACKET));
	int size = NET_STRING_PERPACKET;

	ZeroMemory(&lp, sizeof(lp));
	lp.header = PACKET_HEADER(PACKET_TYPE_TEXT, 0);
	for (int i = 0; i < packets; i++)
	{
		if (i == packets - 1)
			size = len - (buf - old_buf);
		lp.low_id = size;
		memcpy(&(lp.value), buf, size);
		buf += NET_STRING_PERPACKET;
		if (!conmanager.send(&lp, peer))
			return false;
	}

	ZeroMemory(&lp, sizeof(lp));
	lp.header = PACKET_HEADER(PACKET_TYPE_DONE, 0);
	if (!conmanager.send(&lp, peer))
		return false;
	return true;
}

void read_int (int *i)
{
	char str[256];
	char *end;
	long n;
	l(); printf("[%d]> ", *i); u();
	cin.getline(str, 256, '\n');
	n = strtol(str, &end, 10);
	if (str != end)
		*i = n;
}

void spec_handshake (unsigned long peer)
{
		spectator s;
		luna_packet lp;
		char *dummy = "";
		bool ok = true;

		if (!spectators.count(peer))
		{
			l();
			printf("Spectator connecting from %s.\n", ip2str(peer));
			if (DEBUG) fprintf(logfile, "Spectator connecting from %s.\n", ip2str(peer));
			u();
		}

		WaitForSingleObject(mutex_history, INFINITE);
		s.position = 1;
		s.peer = peer;
		s.phase = 0;
		spectators[peer] = s;
		ReleaseMutex(mutex_history);

		// wait until all information can be transmitted
		while (WaitForSingleObject(event_running, 500) == WAIT_TIMEOUT)
		{
			if (WaitForSingleObject(event_waiting, 0) != WAIT_OBJECT_0)
				// sorry, spectators
				return;
		}

		WaitForSingleObject(mutex_history, INFINITE);
		s.phase = 1;
		spectators[peer] = s;
		ReleaseMutex(mutex_history);

		Sleep(100);

		simple_luna_packet(&lp, PACKET_TYPE_RUN, 0);
		ok = ok && conmanager.send(&lp, peer);

		if (strlen(p1_name) && strlen(p2_name))
		{
			ok = ok && send_string(p1_name, peer);
			ok = ok && send_string(p2_name, peer);
		}
		else
		{
			ok = ok && send_string(dummy, peer);
			ok = ok && send_string(dummy, peer);
		}

		simple_luna_packet(&lp, PACKET_TYPE_SEED, 0);
		lp.value = rnd_seed;
		ok = ok && conmanager.send(&lp, peer);

		if (!ok)
		{
			l();
			printf("Connection to spectator %s lost.\n", ip2str(peer));
			u();
			WaitForSingleObject(mutex_history, INFINITE);
			spectators.erase(peer);
			ReleaseMutex(mutex_history);
			conmanager.disconnect(peer);
			return;
		}

		WaitForSingleObject(mutex_history, INFINITE);
		s.phase = 2;
		spectators[peer] = s;
		ReleaseMutex(mutex_history);

		conmanager.set_async(peer);
}

bool spec_accept_callback (SOCKADDR_IN peer, luna_packet *packet)
{
	luna_packet lp;

	conmanager.register_connection(peer, peer_receiver);
	if (!PACKET_IS(*packet, PACKET_TYPE_SPECT) || PACKET_ID(*packet) != 1 || !allow_spectators)
	{
		simple_luna_packet(&lp, PACKET_TYPE_OK, PACKET_ID(*packet));
		conmanager.send_raw(&lp, peer.sin_addr.s_addr, sizeof(lp));
		conmanager.send_raw(&lp, peer.sin_addr.s_addr, sizeof(lp));
		conmanager.send_raw(&lp, peer.sin_addr.s_addr, sizeof(lp));
		simple_luna_packet(&lp, PACKET_TYPE_DENY, 2);
		conmanager.send_raw(&lp, peer.sin_addr.s_addr, sizeof(lp));
		conmanager.send_raw(&lp, peer.sin_addr.s_addr, sizeof(lp));
		conmanager.send_raw(&lp, peer.sin_addr.s_addr, sizeof(lp));
		conmanager.disconnect_nobye(peer.sin_addr.s_addr);
		return false;
	}

	conmanager.rereceive(peer.sin_addr.s_addr, packet);
	CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)spec_handshake, (void *)peer.sin_addr.s_addr, 0, NULL);
	return true;
}

#define CLEANUP_EARLY { if (serve_specs != NULL) { TerminateThread(serve_specs, 0); CloseHandle(serve_specs); } if (alive != NULL) { TerminateThread(alive, 0); CloseHandle(alive); } bye_spectators(); conmanager.clear(); /*clear_socket();*/ close_sems(); closesocket(sock); }
#define TERMINATE_EARLY { CLEANUP_EARLY ; return; }

void spectate_game (char *ip_str, int record_replay)
{
	HANDLE alive = NULL, serve_specs = NULL;
	SOCKADDR_IN sa;
	unsigned long ip = inet_addr(ip_str);
	int seed;
	luna_packet lp;

	ZeroMemory(&sa, sizeof(sa));
	ZeroMemory(&lp, sizeof(lp));

	if (ip == INADDR_NONE)
	{
		l();
		printf("Invalid IP address %s.\n", ip_str);
		u();
		return;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = ip;
	sa.sin_port = htons(port);
	conmanager.init(&sock, port, (DefaultCallback)spec_accept_callback);
	conmanager.register_connection(sa, spec_receiver);

	l(); printf("Connecting to LunaPort on %s.\n", ip_str); u();

	create_sems();

	if (DEBUG) { l(); fprintf(logfile, "Sending SPECT.\n"); u(); }
	simple_luna_packet(&lp, PACKET_TYPE_SPECT, 0);
	if (!conmanager.send(&lp, ip))
	{
		l(); printf("Connection to %s failed.\n", ip_str); u();
		TERMINATE_EARLY
	}

	if (PACKET_IS(lp, PACKET_TYPE_DENY))
	{
		l(); printf("LunaPort at %s does not accept spectators.\n", ip_str); u();
		TERMINATE_EARLY
	}

	alive = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)spec_keep_alive, (void *)ip, 0, NULL);
	serve_specs = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)serve_spectators, NULL, 0, NULL);

	if (!conmanager.receive(&lp, ip))
		TERMINATE_EARLY

	if (PACKET_IS(lp, PACKET_TYPE_ALIVE))
	{
		l(); printf("Waiting for game on %s to start.\n", ip_str); u();
	}

	while (PACKET_IS(lp, PACKET_TYPE_ALIVE))
	{
		if (!conmanager.receive(&lp, ip))
			TERMINATE_EARLY
	}

	if (!PACKET_IS(lp, PACKET_TYPE_RUN))
	{
		l(); printf("Connection to %s terminated.\n", ip_str); u();
		TERMINATE_EARLY
	}

	l(); printf("Game on started. Recieving game data.\n", ip_str); u();

	if (!receive_string(p1_name, ip) || !receive_string(p2_name, ip) || !conmanager.receive(&lp, ip))
	{
		l(); printf("Connection to %s failed.\n", ip_str); u();
		TERMINATE_EARLY
	}

	if (strlen(p1_name) && strlen(p2_name))
	{
		l(); printf("Match: %s vs %s\n", p1_name, p2_name); u();
	}

	set_delay(0);

	if (!PACKET_IS(lp, PACKET_TYPE_SEED))
	{
		l(); printf("Connection to %s failed.\n", ip_str); u();
		TERMINATE_EARLY
	}

	seed = lp.value;
	update_history(seed);
	WaitForSingleObject(mutex_history, INFINITE);
	spec_pos = 1;
	ReleaseMutex(mutex_history);

	if (DEBUG) { l(); fprintf(logfile, "Spectate set up.\n"); u(); }
	l(); printf("Successfully connected to %s. Spectating.\n", ip_str); u();
	remote_player = ip;
	proc = NULL;
	conmanager.set_async(ip);
	if (DEBUG) { l(); fprintf(logfile, "Created spectator threads.\n"); u(); }

	l(); printf("Receiving initial input buffer.\n"); u();
	do
	{
		if (DEBUG) { l(); fprintf(logfile, "Waiting for buffer.\n"); u(); }
		Sleep(126);
	} while (game_history_pos < SPECTATOR_INITIAL);
	l(); printf("Done. Starting game.\n"); u();
	if (DEBUG) { l(); fprintf(logfile, "Running game.\n"); u(); }

	run_game(seed, 0, record_replay, NULL, 1);
	proc = NULL;
	bye_spectators();
	TerminateThread(alive, 0);
	CloseHandle(alive);
	TerminateThread(serve_specs, 0);
	CloseHandle(serve_specs);
	conmanager.clear();
	//clear_socket();
	close_sems();
	closesocket(sock);

	l(); printf("Game finished.\n\n"); u();
}

void join_game (char *ip_str, int record_replay)
{
	HANDLE alive = NULL, serve_specs = NULL;
	SOCKADDR_IN sa;
	int ip = inet_addr(ip_str);
	int seed, d;
	luna_packet lp;

	if (DEBUG) { l(); fprintf(logfile, "Joining\n"); u(); }

	ZeroMemory(&sa, sizeof(sa));
	ZeroMemory(&lp, sizeof(lp));

	if (ip == INADDR_NONE)
	{
		l();
		printf("Invalid IP address %s.\n", ip_str);
		u();
		return;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = ip;
	sa.sin_port = htons(port);
	conmanager.init(&sock, port, (DefaultCallback)spec_accept_callback);
	conmanager.register_connection(sa, peer_receiver);

	l(); printf("Connecting to LunaPort on %s.\n", ip_str); u();

	create_sems();
	simple_luna_packet(&lp, PACKET_TYPE_HELLO, 0);
	if (!conmanager.send(&lp, ip))
	{
		l(); printf("Connection to %s failed.\n", ip_str); u();
		TERMINATE_EARLY
	}

	conmanager.receive(&lp, ip);
	if (PACKET_IS(lp, PACKET_TYPE_PNUM))
	{
		local_p = lp.value ? 1 : 0;
		l(); printf("Establishing connection...\nLocal player %u.\n", local_p+1); u();
	}
	else if (PACKET_IS(lp, PACKET_TYPE_DENY))
	{
		int spec = 0;
		l(); printf("Host is busy. Connection denied.\n"); u();
		CLEANUP_EARLY
		if (ask_spectate)
		{
			do
			{
				l(); printf("Do you want to spectate?\n0: Do not spectate\n1: Spectate\n"); u();
				read_int(&spec);
			} while (spec != 0 && spec != 1);
			if (spec)
			{
				l(); printf("Ok. Entering spectator mode.\n"); u();
				spectate_game(ip_str, record_replay);
			}
			else
			{
				l(); printf("Aborting.\n"); u();
			}
		}
		return;
	}
	else
	{
		l(); printf("Invalid handshake sequence from %s. Expected player number, got %02x.\n", ip2str(ip), PACKET_TYPE_LOW(lp)); u();
		TERMINATE_EARLY
	}

	receive_string(local_p ? p1_name : p2_name, ip);
	strcpy(local_p ? p2_name : p1_name, own_name);
	l(); printf("Remote player: %s\n", local_p ? p1_name : p2_name); u();
	send_string(own_name, ip);

	conmanager.receive(&lp, ip);
	if (PACKET_IS(lp, PACKET_TYPE_SEED))
		seed = lp.value;
	else
	{
		l(); printf("Invalid handshake sequence from %s. Expected random seed, got %02x.\n", ip2str(ip), PACKET_TYPE_LOW(lp)); u();
		TERMINATE_EARLY
	}

	l(); printf("Waiting for delay setting.\n"); u();

	for (;;)
	{
		conmanager.receive(&lp, ip);
		if (PACKET_IS(lp, PACKET_TYPE_PING))
			continue;
		if (!PACKET_IS(lp, PACKET_TYPE_DELAY))
		{
			l(); printf("Invalid handshake sequence from %s. Expected ping or delay, got %02x.\n", ip2str(ip), PACKET_TYPE_LOW(lp)); u();
			TERMINATE_EARLY
		}
		else
			break;
	}

	if (PACKET_IS(lp, PACKET_TYPE_DELAY))
	{
		d = (int)(((float)PACKET_PING(lp) + MS_PER_INPUT) / (2.0 * MS_PER_INPUT) + SAFETY_DELAY);
		if (PACKET_DELAY(lp) == d)
		{
			l(); printf("Determined ping of %ums. This corresponds to an input delay of %u input requests.\nGame setup complete.\n", PACKET_PING(lp), PACKET_DELAY(lp)); u();
		}
		else
		{
			l(); printf("Determined ping of %ums. This corresponds to an input delay of %u input requests.\nDelay set to %u.\nGame setup complete.\n", PACKET_PING(lp), d, PACKET_DELAY(lp)); u();
		}
		set_delay(PACKET_DELAY(lp));
	}
	else
	{
		l(); printf("Invalid handshake sequence from %s. Expected ping or delay, got %02x.\n", ip2str(ip), PACKET_TYPE_LOW(lp)); u();
		TERMINATE_EARLY
	}

	l(); printf("Connected. Starting game.\n"); u();
	remote_player = ip;
	proc = NULL;
	conmanager.set_async(ip);
	alive = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)keep_alive, (void *)remote_player, 0, NULL);
	serve_specs = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)serve_spectators, NULL, 0, NULL);
	run_game(seed, 1, record_replay, NULL, 0);
	proc = NULL;
	conmanager.disconnect(ip);
	bye_spectators();
	TerminateThread(alive, 0);
	CloseHandle(alive);
	TerminateThread(serve_specs, 0);
	CloseHandle(serve_specs);
	conmanager.clear();
	//clear_socket();
	close_sems();
	closesocket(sock);

	l(); printf("Game finished.\n\n"); u();
}

void ping_thread_loop (unsigned long peer)
{
	luna_packet lp;
	simple_luna_packet(&lp, PACKET_TYPE_PING, 0);
	for (;;)
	{
		Sleep(ALIVE_INTERVAL);
		conmanager.send(&lp, peer);
	}
}

void host_game (int seed, int record_replay, int ask_delay)
{
	HANDLE alive = NULL, serve_specs = NULL, ping_thread;
	SOCKADDR_IN sa;
	unsigned long peer = 0;
	luna_packet lp;
	int i, d, tmp;
	DWORD pings = 0;

	if (DEBUG) { l(); fprintf(logfile, "Hosting\n"); u(); }

	ZeroMemory(&sa, sizeof(sa));
	ZeroMemory(&lp, sizeof(lp));

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(port);
	if (bind(sock, (SOCKADDR *)&sa, sizeof(sa)))
	{
		l(); printf("Couldn't bind socket.\n"); u();
		return;
	}

	create_sems();

	conmanager.init(&sock, port, (DefaultCallback)spec_accept_callback);
	serve_specs = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)serve_spectators, NULL, 0, NULL);

	l(); printf("Waiting for connection to LunaPort...\n"); u();
WAIT:
	TerminateThread(serve_specs, 0);
	CloseHandle(serve_specs);
	serve_specs = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)serve_spectators, NULL, 0, NULL);
	conmanager.clear();
	conmanager.init(&sock, port, (DefaultCallback)spec_accept_callback);
	p1_name[0] = 0; p2_name[0] = 0;
	peer = conmanager.accept(peer_receiver);

	if (peer == 0)
		goto WAIT;

	local_p = rand() & 1;
	l(); printf("Establishing connection to %s.\nLocal player %u.\n", ip2str(peer), local_p + 1); u();

	simple_luna_packet(&lp, PACKET_TYPE_PNUM, 0);
	lp.value = local_p ? 0 : 1;
	
	if (!conmanager.send(&lp, peer))
	{
		l(); printf("Failure to establish connection, waiting for new connection, no player number.\n");
		conmanager.disconnect(peer);
		goto WAIT;
	}
	if (!send_string(own_name, peer))
	{
		l(); printf("Failure to establish connection, waiting for new connection, couldn't send name.\n");
		conmanager.disconnect(peer);
		goto WAIT;
	}
	if (!receive_string(local_p ? p1_name : p2_name, peer))
	{
		l(); printf("Failure to establish connection, waiting for new connection, couldn't receive name.\n");
		conmanager.disconnect(peer);
		goto WAIT;
	}
	
	strcpy(local_p ? p2_name : p1_name, own_name);
	l(); printf("Remote player: %s\n", local_p ? p1_name : p2_name); u();

	simple_luna_packet(&lp, PACKET_TYPE_SEED, 0);
	lp.value = seed;
	if (!conmanager.send(&lp, peer))
	{
		l(); printf("Failure to establish connection, waiting for new connection.\n");
		conmanager.disconnect(peer);
		goto WAIT;
	}

	do
	{
		for (i = 0; i < 10; i++)
		{
			tmp = get_ping(peer);
			if (tmp == -1)
			{
				l(); printf("Failure to establish connection, waiting for new connection.\n");
				conmanager.disconnect(peer);
				goto WAIT;
			}
			pings += tmp;
		}
		pings /= 10; // pings/10 = RTT
		d = (int)(((float)pings + MS_PER_INPUT) / (2.0 * MS_PER_INPUT) + SAFETY_DELAY);
		if (d > 0xff)
			d = 0xff;
		if (ask_delay)
		{
			ping_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ping_thread_loop, (void *)peer, 0, NULL);
			l(); printf("\nDetermined ping of %ums. This corresponds to an input delay of %u input requests.\nIf you experience heavy lag, try increasing the delay.\nPlease enter delay ", pings, d); u();
			SetForegroundWindow(FindWindow(NULL, "LunaPort " VERSION));
			read_int(&d);
			TerminateThread(ping_thread, 0);
			CloseHandle(ping_thread);
		}
		else
		{
			l(); printf("\nDetermined ping of %ums. This corresponds to an input delay of %u input requests.\n", pings, d); u();
		}
	} while (d == 0);

	set_delay(d);
	simple_luna_packet(&lp, PACKET_TYPE_DELAY, 0);
	lp.value = PACKET_VALUE_DELAY(delay, pings);
	if (!conmanager.send(&lp, peer))
	{
		l(); printf("Failure to establish connection, waiting for new connection.\n");
		conmanager.disconnect(peer);
		goto WAIT;
	}

	conmanager.set_async(peer);
	remote_player = peer;
	proc = NULL;
	alive = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)keep_alive, (void *)remote_player, 0, NULL);
	run_game(seed, 1, record_replay, NULL, 0);
	proc = NULL;
	conmanager.disconnect(peer);
	bye_spectators();
	TerminateThread(alive, 0);
	CloseHandle(alive);
	TerminateThread(serve_specs, 0);
	CloseHandle(serve_specs);
	conmanager.clear();
	//clear_socket();
	close_sems();
	closesocket(sock);

	l(); printf("Game finished.\n\n"); u();
}

void print_menu (int record_replay)
{
	l();
	printf("LunaPort " VERSION "\n"
		   "====================\n"
		   "1: Host game (port %u UDP)\n"
		   "2: Join game (port %u UDP)\n"
		   "3: Local game with random stages in vs mode\n"
		   "4: Toggle replay recording (%s)\n"
		   "5: Watch replay\n"
		   "6: Toggle spectators (%s)\n"
		   "7: Spectate\n"
		   "8: Display this menu\n"
		   "0: Exit\n"
		   "====================\n\n", port, port, record_replay ? "currently enabled" : "currently disabled", allow_spectators ? "currently allowed" : "currently disabled");
	u();
}

UINT CALLBACK ofn_hook (HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (msg == WM_INITDIALOG)
	{
		SetForegroundWindow(hdlg);
		return TRUE;
	}

	return FALSE;
}

void read_config (unsigned int *port, int *record_replay, int *allow_spectators, unsigned int *max_stages, int *ask_delay,
				  int *ask_spectate, int *display_framerate, int *display_inputrate, int *display_names, char *game_exe,
				  char *own_name)
{
	char config_filename[_MAX_PATH];
	strcpy(config_filename, dir_prefix);
	strcat(config_filename, "\\lunaport.ini");
	*port = GetPrivateProfileInt("LunaPort", "Port", PORT, config_filename);
	*record_replay = GetPrivateProfileInt("LunaPort", "RecordReplayDefault", 0, config_filename);
	*allow_spectators = GetPrivateProfileInt("LunaPort", "AllowSpectatorsDefault", 1, config_filename);
	*max_stages = GetPrivateProfileInt("LunaPort", "Stages", MAX_STAGES, config_filename);
	*ask_delay = GetPrivateProfileInt("LunaPort", "AskDelay", 1, config_filename);
	*ask_spectate = GetPrivateProfileInt("LunaPort", "AskSpectate", 1, config_filename);
	*display_framerate = GetPrivateProfileInt("LunaPort", "DisplayFramerate", 1, config_filename);
	*display_inputrate = GetPrivateProfileInt("LunaPort", "DisplayInputrate", 1, config_filename);
	*display_names = GetPrivateProfileInt("LunaPort", "DisplayNames", 1, config_filename);
	GetPrivateProfileString("LunaPort", "GameExe", GAME, game_exe, _MAX_PATH-1, config_filename);
	GetPrivateProfileString("LunaPort", "PlayerName", "Unknown", own_name, NET_STRING_BUFFER-1, config_filename);
}

int main(int argc, char* argv[])
{
	WSADATA wsa;
	char ip_str[256], tmp[256];
	char filename[_MAX_PATH];
	char replay_dir[_MAX_PATH];
	SECURITY_ATTRIBUTES sa;
	OPENFILENAME ofn;
	int in = 8, old_in = 8;
	int record_replay, ask_delay;
	int i;

	SetConsoleTitle("LunaPort " VERSION);

	if (DEBUG)
	{
		logfile = fopen("debug.log", "w+");
		setvbuf(logfile, NULL, _IONBF, 0);
		fprintf(logfile, "Starting session\n");
	}

	this_proc = GetModuleHandle(NULL);
	local_p = 0;
	srand(time(NULL));
	game_history = NULL;
	game_history_pos = 0;
	ip_str[0] = 0;
	tmp[0] = 0;
	write_replay = NULL;
	recording_ended = false;
	ZeroMemory(game_exe, sizeof(game_exe));
	ZeroMemory(own_name, sizeof(own_name));
	ZeroMemory(p1_name, sizeof(p1_name));
	ZeroMemory(p2_name, sizeof(p2_name));

	ZeroMemory(&sa, sizeof(sa));
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;
	sa.nLength = sizeof(sa);
	mutex_print = CreateMutex(&sa, FALSE, NULL);
	event_running = CreateEvent(&sa, TRUE, FALSE, "");
	event_waiting = CreateEvent(&sa, TRUE, FALSE, "");

	if (argv[0][1] == ':')
	{
		strcpy(dir_prefix, argv[0]);
		for (i = strlen(dir_prefix) - 1; i >= 0; i--)
			if (dir_prefix[i] == '\\')
			{
				dir_prefix[i] = 0;
				break;
			}
	}
	else
		_getcwd(dir_prefix, _MAX_PATH);

	_chdir(dir_prefix);
	strcpy(replay_dir, dir_prefix);
	strcat(replay_dir, "\\Replays\\");

	read_config(&port, &record_replay, &allow_spectators, &max_stages, &ask_delay, &ask_spectate, &display_framerate,
	            &display_inputrate, &display_names, game_exe, own_name);

	if (WSAStartup(MAKEWORD(2, 0), &wsa))
	{
		l(); printf("Couldn't initialize winsocks.\n"); u();
		return 1;
	}

	CreateDirectory("Replays", NULL);

	if (argc == 2)
	{
		if (!strcmp("--local", argv[1]))
		{
			SetEvent(event_waiting);
			run_game((rand()<<16) + rand(), 0, record_replay, NULL, 0);
			printf("\n\nLunaPort done.\n");
			Sleep(1500);
			return 0;
		}
		else
		{
			SetEvent(event_waiting);
			run_game(0, 0, -1, argv[1], 0);
			printf("\n\nLunaPort done.\n");
			Sleep(1500);
			return 0;
		}
	}

	print_menu(record_replay);
	do
	{
		read_int(&in);
		switch (in)
		{
		case 1:
			SetEvent(event_waiting);
			host_game((rand()<<16) + rand(), record_replay, ask_delay);
			break;
		case 2:
			l(); printf("IP[%s]> ", ip_str); u();
			cin.getline(tmp, 256, '\n');
			if (strlen(tmp))
			{
				if (inet_addr(tmp) == INADDR_NONE || inet_addr(tmp) == 0)
				{
					l(); printf("Invalid IP: %s\n", tmp); u();
					break;
				}
				strcpy(ip_str, tmp);
			}
			if (inet_addr(ip_str) == INADDR_NONE || inet_addr(ip_str) == 0)
			{
				l(); printf("Invalid IP: %s\n", ip_str); u();
				break;
			}
			SetEvent(event_waiting);
			join_game(ip_str, record_replay);
			break;
		case 3:
			SetEvent(event_waiting);
			run_game((rand()<<16) + rand(), 0, record_replay, NULL, 0);
			break;
		case 4:
			if (record_replay)
			{
				record_replay = 0;
				printf("Replay recording disabled.\n");
			}
			else
			{
				record_replay = 1;
				printf("Replay recording enabled.\n");
			}
			break;
		case 5:
			ZeroMemory(&ofn, sizeof(ofn));
			ZeroMemory(filename, _MAX_PATH);
			ofn.hwndOwner = GetForegroundWindow();
			ofn.lStructSize = sizeof(ofn);
			ofn.lpstrFilter = "Replay Files (*.rpy)\0*.rpy\0";
			ofn.lpstrFile = filename;
			ofn.nMaxFile = _MAX_PATH-1;
			ofn.lpstrInitialDir = replay_dir;
			ofn.lpstrTitle = "Open Replay";
			ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_ENABLEHOOK | OFN_EXPLORER;
			ofn.lpstrDefExt = "rpy";
			ofn.lpfnHook = ofn_hook;
			if (GetOpenFileName(&ofn))
			{
				_chdir(dir_prefix);
				SetEvent(event_waiting);
				run_game(0, 0, -1, filename, 0);
			}
			else
			{
				printf("No replay selected.\n");
			}
			break;
		case 6:
			if (allow_spectators)
			{
				allow_spectators = 0;
				printf("Spectators will be denied.\n");
			}
			else
			{
				allow_spectators = 1;
				printf("Spectators will be allowed to spectate.\n");
			}
			break;
		case 7:
			l(); printf("IP[%s]> ", ip_str); u();
			cin.getline(tmp, 256, '\n');
			if (strlen(tmp))
			{
				if (inet_addr(tmp) == INADDR_NONE || inet_addr(tmp) == 0)
				{
					l(); printf("Invalid IP: %s\n", tmp); u();
					break;
				}
				strcpy(ip_str, tmp);
			}
			if (inet_addr(ip_str) == INADDR_NONE || inet_addr(ip_str) == 0)
			{
				l(); printf("Invalid IP: %s\n", ip_str); u();
				break;
			}
			SetEvent(event_waiting);
			spectate_game(ip_str, record_replay);
			break;
		case 8:
			print_menu(record_replay);
			break;
		case 0:
			break;
		default:
			l(); printf("Unknown menu item: %d\n", in); u();
			in = old_in;
			break;
		}
		old_in = in;

		if (game_history != NULL)
		{
			free(game_history);
			game_history = NULL;
			game_history_pos = 0;
			spec_pos = 0;
		}
		ResetEvent(event_running);
		ResetEvent(event_waiting);
		spectators.clear();
		p1_name[0] = 0;
		p2_name[0] = 0;
		write_replay = NULL;
		recording_ended = false;
	} while (in != 0);

	WSACleanup();

	printf("\n\nLunaPort done.\n");
	Sleep(300);

	return 0;
}
