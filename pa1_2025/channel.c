// channel.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <time.h>

#pragma comment(lib, "Ws2_32.lib")

#define MAX_CLIENTS 20
#define MAX_FRAME_SIZE 1600  // Enough room for header + payload

#pragma pack(push, 1)
typedef struct {
	uint8_t dst_mac[6];
	uint8_t src_mac[6];
	uint16_t type;      // 0 = DATA, 2 = NOISE
	uint32_t seq_num;
	uint16_t length;
} FrameHeader;
#pragma pack(pop)

int main(int argc, char *argv[]) {
	if (argc != 3) {
		printf("Usage: %s <chan_port> <slot_time_ms>\n", argv[0]);
		return 1;
	}

	int chan_port = atoi(argv[1]);
	int slot_time_ms = atoi(argv[2]);

	// Initialize Winsock
	WSADATA wsaData;
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("Error at WSAStartup()\n");
		return 1;
	}

	// Create listening socket
	SOCKET tcp_s = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_s == INVALID_SOCKET) {
		printf("Error at socket(): %ld\n", WSAGetLastError());
		WSACleanup();
		return 1;
	}

	// Bind socket to port
	struct sockaddr_in my_addr;
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = INADDR_ANY;
	my_addr.sin_port = htons(chan_port);

	int status = bind(tcp_s, (SOCKADDR*)&my_addr, sizeof(my_addr));
	if (status == SOCKET_ERROR) {
		printf("bind() failed: %ld\n", WSAGetLastError());
		closesocket(tcp_s);
		WSACleanup();
		return 1;
	}

	// Start listening
	status = listen(tcp_s, MAX_CLIENTS);
	if (status == SOCKET_ERROR) {
		printf("listen() failed: %ld\n", WSAGetLastError());
		closesocket(tcp_s);
		WSACleanup();
		return 1;
	}

	printf("Channel listening on port %d\n", chan_port);

	// Setup client sockets and stats
	SOCKET client_sockets[MAX_CLIENTS];
	int client_count = 0;
	int total_frames_received[MAX_CLIENTS] = { 0 };
	int collision_count[MAX_CLIENTS] = { 0 };
	time_t first_frame_time[MAX_CLIENTS] = { 0 };
	time_t last_frame_time[MAX_CLIENTS] = { 0 };
	int64_t total_bytes[MAX_CLIENTS] = { 0 };

	char buffer[MAX_FRAME_SIZE];
	HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
	struct sockaddr_in peer_addr;
	int peer_addr_len = sizeof(peer_addr);

	while (1) {
		// Accept one new connection if possible
		if (client_count < MAX_CLIENTS) {
			SOCKET new_socket = accept(tcp_s, (SOCKADDR*)&peer_addr, &peer_addr_len);
			if (new_socket != INVALID_SOCKET) {
				client_sockets[client_count++] = new_socket;
				printf("New server connected. Total: %d\n", client_count);
			}
		}

		// Check if user pressed Ctrl+Z
		if (WaitForSingleObject(hStdin, 0) == WAIT_OBJECT_0) {
			char ch;
			DWORD read;
			ReadConsoleA(hStdin, &ch, 1, &read, NULL);
			if (read > 0 && ch == 26) {  // ASCII 26 = Ctrl+Z
				break;
			}
		}

		Sleep(slot_time_ms); // ALOHA time slot

		// Check which sockets are ready to read
		fd_set readfds;
		FD_ZERO(&readfds);
		for (int i = 0; i < client_count; i++) {
			FD_SET(client_sockets[i], &readfds);
		}

		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;  // No wait

		int ready_count = select(0, &readfds, NULL, NULL, &timeout);

		int received_count = 0;
		SOCKET senders[MAX_CLIENTS];
		int send_lengths[MAX_CLIENTS];

		// Read frames from ready sockets
		for (int i = 0; i < client_count; i++) {
			if (FD_ISSET(client_sockets[i], &readfds)) {
				int bytes = recv(client_sockets[i], buffer, MAX_FRAME_SIZE, 0);
				if (bytes >= sizeof(FrameHeader)) {
					senders[received_count] = client_sockets[i];
					send_lengths[received_count] = bytes;
					total_frames_received[i]++;
					received_count++;

					if (first_frame_time[i] == 0) {
						first_frame_time[i] = time(NULL);
					}
					last_frame_time[i] = time(NULL);
					total_bytes[i] += bytes;
				}
			}
		}

		// If one sender — broadcast frame
		if (received_count == 1) {
			for (int i = 0; i < client_count; i++) {
				send(client_sockets[i], buffer, send_lengths[0], 0);
			}
		}
		// If collision — broadcast noise
		else if (received_count > 1) {
			FrameHeader noise;
			memset(&noise, 0, sizeof(noise));
			noise.type = 2; // NOISE

			for (int i = 0; i < client_count; i++) {
				send(client_sockets[i], (char*)&noise, sizeof(noise), 0);
			}

			// Update collision counts
			for (int k = 0; k < received_count; k++) {
				for (int j = 0; j < client_count; j++) {
					if (senders[k] == client_sockets[j]) {
						collision_count[j]++;
						break;
					}
				}
			}
		}
	}

	// --- EOF Detected: Print Stats ---a
	for (int i = 0; i < client_count; i++) {
		struct sockaddr_in addr;
		int len = sizeof(addr);
		getpeername(client_sockets[i], (struct sockaddr*)&addr, &len);

		double bandwidth = 0.0;
		if (total_frames_received[i] > 0 && first_frame_time[i] != last_frame_time[i]) {
			double duration = difftime(last_frame_time[i], first_frame_time[i]);
			if (duration > 0) {
				bandwidth = (total_bytes[i] * 8.0) / (duration * 1000000); // Mbps
			}
		}

		fprintf(stderr, "From %s port %d: %d frames, %d collisions, %.3f Mbps\n",
			inet_ntoa(addr.sin_addr),
			ntohs(addr.sin_port),
			total_frames_received[i],
			collision_count[i],
			bandwidth
		);
	}

	// --- Cleanup ---
	for (int i = 0; i < client_count; i++) {
		closesocket(client_sockets[i]);
	}

	closesocket(tcp_s);
	WSACleanup();
	return 0;
}
