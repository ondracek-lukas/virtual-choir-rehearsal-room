// Virtual Choir Rehearsal Room  Copyright (C) 2020  Lukas Ondracek <ondracek.lukas@gmail.com>, use under GNU GPLv3

#include "main.h"

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>

#include "audioBuffer.h"
#include "surround.h"
#include "net.h"
#include "tty.h"

struct client {
	bool connected;
	uint8_t id;
	struct sockaddr_storage addr;
	int64_t lastPacketUsec;
	float aioLatency;
	float restLatency;
	float dBAdj;
	bindex_t lastKeyPress;
	sample_t lastReadBlock[STEREO_BLOCK_SIZE];
	struct surroundCtx surroundCtx;
	struct audioBuffer buffer;
	char name[NAME_LEN + 1];
};
struct client *clients[MAX_CLIENTS];
struct client *clientsOrdered[MAX_CLIENTS]; // may temporarily contain duplicit/missing items

int udpSocket = -1;
pthread_t udpThread;
bindex_t blockIndex = 0;
bindex_t statusIndex = 0;
int64_t usecZero;

int64_t getUsec(int64_t zero) {
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
	return tp.tv_sec * 1000000ull + tp.tv_nsec/1000 - zero;
}


volatile enum udpState {
	UDP_OPEN,
	UDP_CLOSED
} udpState;

#define msg(...) msg2(__VA_ARGS__, "")
#define msg2(fmt, ...) { \
	int64_t usec = getUsec(usecZero); \
	int64_t ms = usec / 1000; \
	int64_t s = ms / 1000; \
	int64_t m = s / 60; \
	int64_t h = m / 60; \
	ms -= 1000 * s; s -= 60 * m; m -= 60 * h; \
	printf("[%02d:%02d:%02d.%03d] " fmt "%s\n", h, m, s, ms, __VA_ARGS__); }

struct client *getClient(size_t c) {
	if (!clients[c] || !clients[c]->connected) return NULL;
	return clients[c];
}

#define FOR_CLIENTS(CLIENT) \
	for (size_t CLIENT##_INDEX = 0; CLIENT##_INDEX < MAX_CLIENTS; CLIENT##_INDEX++) \
	for (struct client *CLIENT = getClient(CLIENT##_INDEX); CLIENT; CLIENT=NULL)

#define FOR_CLIENTS_ORDERED(CLIENT) \
	for (size_t CLIENT##_INDEX = 0; CLIENT##_INDEX < MAX_CLIENTS; CLIENT##_INDEX++) \
	for (struct client *CLIENT = clientsOrdered[CLIENT##_INDEX]; CLIENT && CLIENT->connected; CLIENT=NULL)

void clientsSurroundReinit() {
	size_t clientsCnt = 0;
	FOR_CLIENTS(client) clientsCnt++;

	size_t i = 0;
	FOR_CLIENTS_ORDERED(client) {
		surroundInitCtx(&client->surroundCtx, client->dBAdj, M_PI * ((float)i++ / (clientsCnt-1) - 0.5f), 2);
	}
}

struct client *newClient() {
	struct client *client = NULL;
	for (ssize_t i = 0; i < MAX_CLIENTS; i++) {
		if (!clients[i]) {
			client = malloc(sizeof(struct client));
			if (!client) {
				msg("Cannot allocate memory for a new client, refusing...");
				return NULL;
			}
			client->id = i;
			client->connected = false;
			__sync_synchronize();
			clients[i] = client;
			break;
		} else if (!clients[i]->connected) {
			client = clients[i];
			break;
		}
	}

	if (!client) {
		msg("Max number of clients (%d) exceeded, refusing new connection...", MAX_CLIENTS);
		return NULL;
	}

	size_t i = 0;
	for (size_t j = 0; j < MAX_CLIENTS; j++) {
		if (clientsOrdered[j] && clientsOrdered[j]->connected && (clientsOrdered[j] != client)) {
			if (i < j) {
				clientsOrdered[i] = clientsOrdered[j];
			}
			i++;
		}
	}
	clientsOrdered[i++] = client;
	for (; i < MAX_CLIENTS; i++) {
		clientsOrdered[i] = NULL;
	}
	__sync_synchronize();
	return client;
}

void clientMoveUp(struct client *client) {
	ssize_t i = -1;
	for (ssize_t j = 0; j < MAX_CLIENTS; j++) {
		if (clientsOrdered[j] && clientsOrdered[j]->connected) {
			if (clientsOrdered[j] == client) {
				if (i >= 0) {
					clientsOrdered[j] = clientsOrdered[i];
					clientsOrdered[i] = client;
				}
				break;
			}
			i = j;
		}
	}
	clientsSurroundReinit();
}

void clientMoveDown(struct client *client) {
	ssize_t i = -1;
	for (ssize_t j = 0; j < MAX_CLIENTS; j++) {
		if (clientsOrdered[j] && clientsOrdered[j]->connected) {
			if (i >= 0) {
				clientsOrdered[i] = clientsOrdered[j];
				clientsOrdered[j] = client;
				break;
			} else if (clientsOrdered[j] == client) {
				i = j;
			}
		}
	}
	clientsSurroundReinit();
}

ssize_t udpSendPacket(struct client *client, void *packet, size_t size) {
	return sendto(udpSocket, packet, size, 0, (struct sockaddr *)&client->addr, sizeof(client->addr));
}

void udpRecvHelo(struct client *client, struct packetClientHelo *packet) {
	strcpy(client->name, packet->name);
	bufferClear(&client->buffer, 0);
	client->lastPacketUsec = getUsec(usecZero);
	client->aioLatency = packet->aioLatency;
	client->dBAdj = packet->dBAdj;
	surroundInitCtx(&client->surroundCtx, client->dBAdj, 0, 2);
	bufferOutputStatsReset(&client->buffer, true);
	client->lastKeyPress = 0;
	__sync_synchronize();
	client->connected = true;

	struct packetServerHelo packetR;
	packetR.clientID = client->id;
	packetR.initBlockIndex = blockIndex;
	strncpy(packetR.str, "du\n" // "dumjkhln\n"
		"[d/u] move down/up in list",
//		"[m]   turn metronome on/off\n"
//		"[j/k] decrease/increase beats per minute\n"
//		"[h/l] decrease/increase beats per bar\n"
//		"[n]   set metronome by multiple presses in rhythm and turn it on",
		SHELO_STR_LEN);

	udpSendPacket(client, &packetR, (void *)strchr(packetR.str, '\0') - (void *)&packetR);
	clientsSurroundReinit();
}

void udpRecvData(struct client *client, struct packetClientData *packet) {
	client->restLatency = (float) MONO_BLOCK_SIZE / SAMPLE_RATE * 1000 *
		((int)blockIndex - packet->playBlockIndex + packet->blockIndex - client->buffer.readPos); // XXX check
	bufferWrite(&client->buffer, packet->blockIndex, packet->block);
	client->lastPacketUsec = getUsec(usecZero);
}

void udpRecvKeyPress(struct client *client, struct packetKeyPress *packet) {
	switch (packet->key) {
		case 'u': // move up
			clientMoveUp(client);
			break;

		case 'd': // move down
			clientMoveDown(client);
			break;

		case 'm': // toggle metronome
		case 'j': // decrease beats per minute
		case 'k': // increase beats per minute
		case 'h': // decrease beats per bar
		case 'l': // increase beats per bar
		case 'n': // multiple-press metronome activation
			break;
	}
}

void *udpReceiver(void *none) {
	char packetRaw[sizeof(union packet) + 1];
	union packet *packet = (union packet *) &packetRaw;
	ssize_t size;
	struct sockaddr_storage addr;
	struct client *client;
	socklen_t addr_len = sizeof(addr);

	while ((size = recvfrom(udpSocket, packetRaw, sizeof(union packet), 0, (struct sockaddr *)&addr, &addr_len)) >= 0) {
		switch (packetRaw[0]) {
			case PACKET_HELO:
				packetRaw[size] = '\0';
				if (packet->cHelo.version != PROT_VERSION) {
					msg("Different version connection refused (%d instead %d)...", packet->cHelo.version, PROT_VERSION);
					break;
				}
				packet->cHelo.name[NAME_LEN] = '\0';

				{
					bool duplicate = false;
					FOR_CLIENTS(client2) {
						if (netAddrsEqual(&client2->addr, &addr)) {
							msg("Second helo packet from the same address refused...");
							duplicate = true;
							break;
						}
					}
					if (duplicate) break;
				}
				if (!(client = newClient())) {
					break;
				}
				client->addr = addr;
				udpRecvHelo(client, &packet->cHelo);
				msg("New client '%s' with id %d accepted...", client->name, client->id);
				break;
			case PACKET_DATA:
				if (
						(size != sizeof(struct packetClientData)) ||
						!(client = getClient(packet->cData.clientID)) ||
						!netAddrsEqual(&addr, &client->addr)
					) break;
				udpRecvData(client, &packet->cData);
				break;
			case PACKET_KEY_PRESS:
				if (
						(size != sizeof(struct packetKeyPress)) ||
						!(client = getClient(packet->cKeyP.clientID)) ||
						!netAddrsEqual(&addr, &client->addr) ||
						(client->lastKeyPress >= packet->cKeyP.playBlockIndex)
					) break;
				client->lastKeyPress = packet->cKeyP.playBlockIndex;
				msg("Key '%c' pressed by '%s'...", packet->cKeyP.key, client->name);
				udpRecvKeyPress(client, &packet->cKeyP);
				break;
		}
		__sync_synchronize();
		addr_len = sizeof(addr);
	}
	msg("UDP receiver error.");
	udpState = UDP_CLOSED;
}

int64_t getBlockUsec(bindex_t index) {
	return (int64_t)index * 1000000 * MONO_BLOCK_SIZE / SAMPLE_RATE;
}

void getStatusStr(char **s, struct client *client) {
	if (client->aioLatency > 0) {
		*s += sprintf(*s, " %-10s%3.0f+%-4.0fms ",
			client->name, client->aioLatency, client->restLatency);
	} else {
		*s += sprintf(*s, " %-10s  ?+%-4.0fms ",
			client->name, client->restLatency);
	}
	float avg, peak;
	bufferOutputStats(&client->buffer, &avg, &peak);
	ttyFormatSndLevel(s, avg + client->dBAdj, peak + client->dBAdj);
	*(*s)++ = '\n';
}

#define ERR(...) {msg(__VA_ARGS__); return 1; }
int main() {
	netInit();
	udpSocket = netOpenPort(STR(UDP_PORT));
	if (udpSocket < 0) {
		ERR("Cannot open port.");
	}

	udpState = UDP_OPEN;
	pthread_create(&udpThread, NULL, &udpReceiver, NULL);

	usecZero = getUsec(0);
	int64_t usecFreeSum = 0;
	struct packetServerData packet = { .type = PACKET_DATA };

	while (udpState == UDP_OPEN) {
		__sync_synchronize();

		// sound mixing [

		sample_t mixedBlock[STEREO_BLOCK_SIZE];
		sample_t *block = packet.block;
		memset(mixedBlock, 0, STEREO_BLOCK_SIZE * sizeof(sample_t));
		packet.blockIndex = blockIndex;
		FOR_CLIENTS(client) {
			sample_t *clientBlock = client->lastReadBlock;
			surroundFilter(&client->surroundCtx, bufferReadNext(&client->buffer), clientBlock);
			for (size_t i = 0; i < STEREO_BLOCK_SIZE; i++) {
				mixedBlock[i] += clientBlock[i];
			}
		}
		FOR_CLIENTS(client) {
			sample_t *clientBlock = client->lastReadBlock;
			for (size_t i = 0; i < STEREO_BLOCK_SIZE; i++) {
				block[i] = mixedBlock[i] - clientBlock[i];
			}
			ssize_t err = udpSendPacket(client, &packet, sizeof(struct packetServerData));
			// ssize_t err = sendto(udpSocket, &packet, sizeof(struct packetServerData), 0,
			// 	(struct sockaddr *)&clients[c]->addr, sizeof(struct sockaddr_storage));
			if (err < 0) {
				msg("Sending to client %d '%s' failed, disconnected...", client->id, client->name);
				client->connected = false;
			}
		}

		// ] end of sound mixing


		int64_t usec = getUsec(usecZero);
		int64_t usecWait = getBlockUsec(++blockIndex) - usec;
		usecFreeSum += usecWait;
		if (blockIndex % 50 == 0) {
			FOR_CLIENTS(client) {
				if (usec - client->lastPacketUsec > 1000000) {
					client->connected = false;
					msg("Client %d '%s' timeout, disconnected...", client->id, client->name);
				}
			}
		}
		if (blockIndex % BLOCKS_PER_STAT == 0) {
			struct packetStatusStr packet = {
				.type = PACKET_STATUS,
				.packetsCnt = 2,
				.packetIndex = 0,
				.statusIndex = statusIndex++};

			struct clientsLines {
				struct client *client;
				char *s;
			} clientsLines[STATUS_LINES_PER_PACKET];

			FOR_CLIENTS(client) packet.packetsCnt++;
			packet.packetsCnt = (packet.packetsCnt + STATUS_LINES_PER_PACKET - 1) / STATUS_LINES_PER_PACKET;

			char *s = packet.str;
			size_t l = 0;
			clientsLines[0] = (struct clientsLines) { NULL, s };
			s += sprintf(s, "---------------------  left\n"); l++;
			FOR_CLIENTS_ORDERED(client) {
				clientsLines[l % STATUS_LINES_PER_PACKET] = (struct clientsLines) {client, s};
				getStatusStr(&s, client);

				if (++l % STATUS_LINES_PER_PACKET == 0) {
					*s = '\0';
					FOR_CLIENTS(clientReceiver) {
						ssize_t clientLine = -1;
						for (size_t i = 0; i < STATUS_LINES_PER_PACKET; i++) {
							if (clientsLines[i].client == clientReceiver) {
								clientLine = i;
								clientsLines[i].s[0] = '.';
								break;
							}
						}
						udpSendPacket(clientReceiver, &packet, (void *)s - (void *)&packet);
						if (clientLine >= 0) {
							clientsLines[clientLine].s[0] = ' ';
						}
					}
					packet.packetIndex++;
					s = packet.str;
				}
			}
			s += sprintf(s, "---------------------  right\n");
			*s = '\0';
			FOR_CLIENTS(clientReceiver) {
				ssize_t clientLine = -1;
				for (size_t i = 0; i < (l % STATUS_LINES_PER_PACKET); i++) {
					if (clientsLines[i].client == clientReceiver) {
						clientLine = i;
						clientsLines[i].s[0] = '.';
						break;
					}
				}
				udpSendPacket(clientReceiver, &packet, (void *)s - (void *)&packet);
				if (clientLine >= 0) {
					clientsLines[clientLine].s[0] = ' ';
				}
			}

		}
		if (blockIndex % 1000 == 0) {
			msg("Sound mixer load: %6.2f %%", (float)(getBlockUsec(1000) - usecFreeSum)/getBlockUsec(1000) * 100);
			usecFreeSum = 0;
		}
		__sync_synchronize();
		if (usecWait > 0) {
			usleep(usecWait);
		} else {
			msg("Sound mixer was late by %lld us...", -usecWait);
		}
	}

	pthread_join(udpThread, NULL);
	netCleanup();
	msg("Exitting...");

	return 0;
 }
