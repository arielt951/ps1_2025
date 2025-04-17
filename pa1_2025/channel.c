//channel.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <time.h>
#include <stdbool.h>

#pragma comment(lib, "Ws2_32.lib")

#define MAX_CLIENTS 20
#define MAX_FRAME_SIZE 1600  // Enough room for header + payload

#pragma pack(push, 1)
typedef struct { //header struct
	uint8_t src_mac[6];
	uint8_t dst_mac[6];
	uint16_t type;      // 0 = DATA, 2 = NOISE
	uint32_t seq_num;
	uint16_t length;
} FrameHeader;
#pragma pack(pop)

typedef struct {
	SOCKET socket;
	struct sockaddr_in addr;
	int total_frames;
	int collision_count;
	clock_t first_frame_time;
	clock_t last_frame_time;
	int64_t total_bytes;
	bool active;
} ClientInfo;

// Function to create a noise frame
void create_noise_frame(char* buffer) {
	FrameHeader* noise = (FrameHeader*)buffer;
	memset(noise, 0, sizeof(FrameHeader));
	noise->type = 2; // NOISE
}

// Function to check for user input (Ctrl+Z)
bool check_for_exit() {
	HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
	DWORD numEvents = 0;

	GetNumberOfConsoleInputEvents(hStdin, &numEvents);
	if (numEvents > 0) {
		INPUT_RECORD inRec;
		DWORD numRead;

		PeekConsoleInput(hStdin, &inRec, 1, &numRead);
		if (numRead > 0 && inRec.EventType == KEY_EVENT &&
			inRec.Event.KeyEvent.bKeyDown &&
			inRec.Event.KeyEvent.wVirtualKeyCode == 'Z' &&
			(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
			FlushConsoleInputBuffer(hStdin);
			return true;
		}
	}
	return false;
}

// Function to find client index by socket
int find_client_by_socket(ClientInfo* clients, int client_count, SOCKET s) {
	for (int i = 0; i < client_count; i++) {
		if (clients[i].active && clients[i].socket == s) {
			return i;
		}
	}
	return -1;
}

// Function to cleanup and free resources
void cleanup(SOCKET tcp_s, ClientInfo* clients, int client_count) {
	for (int i = 0; i < client_count; i++) {
		if (clients[i].active) {
			closesocket(clients[i].socket);
		}
	}
	closesocket(tcp_s);
	WSACleanup();
}

int main(int argc, char *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <chan_port> <slot_time_ms>\n", argv[0]);
		return 1;
	}

	int chan_port = atoi(argv[1]);
	int slot_time_ms = atoi(argv[2]);

	// Initialize Winsock
	WSADATA wsaData;
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		fprintf(stderr, "Error at WSAStartup(): %d\n", iResult);
		return 1;
	}

	// Create listening socket
	SOCKET tcp_s = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_s == INVALID_SOCKET) {
		fprintf(stderr, "Error at socket(): %ld\n", WSAGetLastError());
		WSACleanup();
		return 1;
	}

	// Set socket to non-blocking mode
	u_long mode = 1;
	ioctlsocket(tcp_s, FIONBIO, &mode);

	// Bind socket to port
	struct sockaddr_in my_addr;
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = INADDR_ANY;
	my_addr.sin_port = htons(chan_port);

	int status = bind(tcp_s, (struct sockaddr*)&my_addr, sizeof(my_addr));
	if (status == SOCKET_ERROR) {
		fprintf(stderr, "bind() failed: %ld\n", WSAGetLastError());
		closesocket(tcp_s);
		WSACleanup();
		return 1;
	}

	// Start listening
	status = listen(tcp_s, MAX_CLIENTS);
	if (status == SOCKET_ERROR) {
		fprintf(stderr, "listen() failed: %ld\n", WSAGetLastError());
		closesocket(tcp_s);
		WSACleanup();
		return 1;
	}

	printf("Channel listening on port %d\n", chan_port);

	// Setup client tracking
	ClientInfo clients[MAX_CLIENTS];
	int client_count = 0;

	// Initialize client info
	for (int i = 0; i < MAX_CLIENTS; i++) {
		clients[i].active = false;
	}

	// Buffers for frame storage
	char* frame_buffers[MAX_CLIENTS];
	int frame_lengths[MAX_CLIENTS];

	// Allocate frame buffers
	for (int i = 0; i < MAX_CLIENTS; i++) {
		frame_buffers[i] = (char*)malloc(MAX_FRAME_SIZE);
		if (!frame_buffers[i]) {
			fprintf(stderr, "Memory allocation failed\n");
			cleanup(tcp_s, clients, client_count);
			return 1;
		}
	}

	// Main channel loop
	bool running = true;
	while (running) {
		// Check for exit command (Ctrl+Z)
		if (check_for_exit()) {
			running = false;
			break;
		}

		// Setup for select() on listening socket and client sockets
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(tcp_s, &readfds);

		for (int i = 0; i < client_count; i++) {
			if (clients[i].active) {
				FD_SET(clients[i].socket, &readfds);
			}
		}

		// Wait with timeout
		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = slot_time_ms * 1000; // Convert ms to μs

		int ready_count = select(0, &readfds, NULL, NULL, &timeout);

		// Check for accept on listening socket
		if (FD_ISSET(tcp_s, &readfds)) {
			struct sockaddr_in peer_addr;
			int peer_addr_len = sizeof(peer_addr);

			SOCKET new_socket = accept(tcp_s, (struct sockaddr*)&peer_addr, &peer_addr_len);
			if (new_socket != INVALID_SOCKET) {
				// Set new socket to non-blocking
				u_long mode = 1;
				ioctlsocket(new_socket, FIONBIO, &mode);

				// Find an empty slot
				int slot = -1;
				for (int i = 0; i < MAX_CLIENTS; i++) {
					if (!clients[i].active) {
						slot = i;
						break;
					}
				}

				if (slot >= 0) {
					clients[slot].socket = new_socket;
					clients[slot].addr = peer_addr;
					clients[slot].total_frames = 0;
					clients[slot].collision_count = 0;
					clients[slot].first_frame_time = 0;
					clients[slot].last_frame_time = 0;
					clients[slot].total_bytes = 0;
					clients[slot].active = true;

					if (slot >= client_count) {
						client_count = slot + 1;
					}

					printf("New server connected from %s:%d (slot %d)\n",
						inet_ntoa(peer_addr.sin_addr),
						ntohs(peer_addr.sin_port),
						slot);
				}
				else {
					fprintf(stderr, "Maximum number of clients reached, rejecting connection\n");
					closesocket(new_socket);
				}
			}
		}

		// Count received frames in this slot
		int frames_received = 0;
		int sender_indices[MAX_CLIENTS];

		// Check client sockets for data
		for (int i = 0; i < client_count; i++) {
			if (clients[i].active && FD_ISSET(clients[i].socket, &readfds)) {
				int bytes = recv(clients[i].socket, frame_buffers[frames_received], MAX_FRAME_SIZE, 0);

				if (bytes > 0 && bytes >= sizeof(FrameHeader)) {
					frame_lengths[frames_received] = bytes;
					sender_indices[frames_received] = i;
					frames_received++;

					// Update statistics
					clients[i].total_frames++;
					if (clients[i].first_frame_time == 0) {
						clients[i].first_frame_time = clock();
					}
					clients[i].last_frame_time = clock();
					clients[i].total_bytes += bytes;
				}
				else if (bytes == 0 || (bytes == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
					// Client disconnected or error
					printf("Client disconnected: %s:%d\n",
						inet_ntoa(clients[i].addr.sin_addr),
						ntohs(clients[i].addr.sin_port));

					closesocket(clients[i].socket);
					clients[i].active = false;
				}
			}
		}

		// Process received frames
		if (frames_received == 1) {
			// No collision - broadcast the frame to all clients
			for (int i = 0; i < client_count; i++) {
				if (clients[i].active) {
					int sent = send(clients[i].socket, frame_buffers[0], frame_lengths[0], 0);
					if (sent == SOCKET_ERROR) {
						int err = WSAGetLastError();
						if (err != WSAEWOULDBLOCK) {
							fprintf(stderr, "Error sending to client %d: %d\n", i, err);
							closesocket(clients[i].socket);
							clients[i].active = false;
						}
					}
				}
			}
		}
		else if (frames_received > 1) {
			// Collision - send noise and update collision counts
			char noise_buffer[sizeof(FrameHeader)];
			create_noise_frame(noise_buffer);

			for (int i = 0; i < client_count; i++) {
				if (clients[i].active) {
					int sent = send(clients[i].socket, noise_buffer, sizeof(FrameHeader), 0);
					if (sent == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
						closesocket(clients[i].socket);
						clients[i].active = false;
					}
				}
			}

			// Update collision statistics
			for (int k = 0; k < frames_received; k++) {
				clients[sender_indices[k]].collision_count++;
			}
		}
	}

	// Print statistics after Ctrl+Z
	for (int i = 0; i < client_count; i++) {
		if (clients[i].active) {
			double bandwidth_mbps = 0.0;
			if (clients[i].total_frames > 0 &&
				clients[i].first_frame_time != 0 &&
				clients[i].last_frame_time > clients[i].first_frame_time) {

				double duration_sec = (double)(clients[i].last_frame_time - clients[i].first_frame_time) / CLOCKS_PER_SEC;
				if (duration_sec > 0) {
					bandwidth_mbps = (clients[i].total_bytes * 8.0) / (duration_sec * 1000000); // Mbps
				}
			}

			fprintf(stderr, "From %s port %d: %d frames, %d collisions\n",
				inet_ntoa(clients[i].addr.sin_addr),
				ntohs(clients[i].addr.sin_port),
				clients[i].total_frames,
				clients[i].collision_count);

			fprintf(stderr, "Average bandwidth: %.3f Mbps\n", bandwidth_mbps);
		}
	}

	// Clean up and free resources
	for (int i = 0; i < MAX_CLIENTS; i++) {
		free(frame_buffers[i]);
	}

	cleanup(tcp_s, clients, client_count);
	return 0;
}