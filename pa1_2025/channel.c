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

// Linked list node for client management
typedef struct ClientNode {
	ClientInfo info;
	char* frame_buffer;
	int frame_length;
	struct ClientNode* next;
} ClientNode;

typedef struct {
	struct sockaddr_in addr;
	int total_frames;
	int collision_count;
	clock_t first_frame_time;
	clock_t last_frame_time;
	int64_t total_bytes;
} ServerArchive;
// Linked list node for archived servers
typedef struct ArchiveNode {
	ServerArchive info;
	struct ArchiveNode* next;
} ArchiveNode;

// Global linked list head
static ClientNode* client_list = NULL;
static int client_count = 0;
static ArchiveNode* archive_list = NULL;

// Forward declarations
void archive_server(ClientNode* client);
void print_all_statistics();

// Function to initialize Winsock and create a listening socket
SOCKET create_listening_socket(int port) {
	// Initialize Winsock
	WSADATA wsaData;
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		fprintf(stderr, "Error at WSAStartup(): %d\n", iResult);
		return INVALID_SOCKET;
	}

	// Create listening socket
	SOCKET tcp_s = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_s == INVALID_SOCKET) {
		fprintf(stderr, "Error at socket(): %ld\n", WSAGetLastError());
		WSACleanup();
		return INVALID_SOCKET;
	}

	// Set socket to non-blocking mode
	u_long mode = 1;
	ioctlsocket(tcp_s, FIONBIO, &mode);

	// Bind socket to port
	struct sockaddr_in my_addr;
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = INADDR_ANY;
	my_addr.sin_port = htons(port);

	int status = bind(tcp_s, (struct sockaddr*)&my_addr, sizeof(my_addr));
	if (status == SOCKET_ERROR) {
		fprintf(stderr, "bind() failed: %ld\n", WSAGetLastError());
		closesocket(tcp_s);
		WSACleanup();
		return INVALID_SOCKET;
	}

	// Start listening
	status = listen(tcp_s, 10); // Allow up to 10 pending connections
	if (status == SOCKET_ERROR) {
		fprintf(stderr, "listen() failed: %ld\n", WSAGetLastError());
		closesocket(tcp_s);
		WSACleanup();
		return INVALID_SOCKET;
	}

	return tcp_s;
}

// Function to add a server to the archive when it disconnects
void archive_server(ClientNode* client) {
	// Create a new archive node
	ArchiveNode* new_archive = (ArchiveNode*)malloc(sizeof(ArchiveNode));
	if (!new_archive) {
		fprintf(stderr, "Memory allocation failed for archive node\n");
		return;
	}

	// Copy server information to the archive
	new_archive->info.addr = client->info.addr;
	new_archive->info.total_frames = client->info.total_frames;
	new_archive->info.collision_count = client->info.collision_count;
	new_archive->info.first_frame_time = client->info.first_frame_time;
	new_archive->info.last_frame_time = client->info.last_frame_time;
	new_archive->info.total_bytes = client->info.total_bytes;

	// Add to the beginning of the archive list
	new_archive->next = archive_list;
	archive_list = new_archive;

	printf("Server %s:%d archived (will print stats at exit)\n",
		inet_ntoa(client->info.addr.sin_addr),
		ntohs(client->info.addr.sin_port));
}

// Create a new client node and add it to the linked list
ClientNode* add_client(SOCKET socket, struct sockaddr_in addr) {
	// Create a new client node
	ClientNode* new_client = (ClientNode*)malloc(sizeof(ClientNode));
	if (!new_client) {
		fprintf(stderr, "Memory allocation failed for client node\n");
		return NULL;
	}

	// Allocate frame buffer
	new_client->frame_buffer = (char*)malloc(MAX_FRAME_SIZE);
	if (!new_client->frame_buffer) {
		fprintf(stderr, "Memory allocation failed for frame buffer\n");
		free(new_client);
		return NULL;
	}

	// Initialize client info
	new_client->info.socket = socket;
	new_client->info.addr = addr;
	new_client->info.total_frames = 0;
	new_client->info.collision_count = 0;
	new_client->info.first_frame_time = 0;
	new_client->info.last_frame_time = 0;
	new_client->info.total_bytes = 0;
	new_client->info.active = true;
	new_client->frame_length = 0;

	// Add to the beginning of the list (O(1) operation)
	new_client->next = client_list;
	client_list = new_client;
	client_count++;

	printf("New server connected from %s:%d\n",
		inet_ntoa(addr.sin_addr),
		ntohs(addr.sin_port));

	return new_client;
}

// Find a client node by socket
ClientNode* find_client_by_socket(SOCKET socket) {
	ClientNode* current = client_list;
	while (current != NULL) {
		if (current->info.active && current->info.socket == socket) {
			return current;
		}
		current = current->next;
	}
	return NULL;
}

//   function to archive before removing
void remove_client(SOCKET socket) {
	ClientNode* current = client_list;
	ClientNode* prev = NULL;

	while (current != NULL) {
		if (current->info.socket == socket) {
			// Archive this client's statistics before removal
			if (current->info.total_frames > 0) {
				archive_server(current);
			}

			// Close the socket
			closesocket(socket);

			// Remove from the list
			if (prev == NULL) {
				// It's the head of the list
				client_list = current->next;
			}
			else {
				prev->next = current->next;
			}

			// Free resources
			free(current->frame_buffer);
			free(current);
			client_count--;
			return;
		}

		prev = current;
		current = current->next;
	}
}
// Cleanup all clients and free resources
void cleanup_clients() {
	ClientNode* current = client_list;
	ClientNode* next;

	while (current != NULL) {
		next = current->next;

		if (current->info.active) {
			closesocket(current->info.socket);
		}

		free(current->frame_buffer);
		free(current);

		current = next;
	}

	client_list = NULL;
	client_count = 0;
}

// Create a noise frame to indicate collision
void create_noise_frame(char* buffer) {
	FrameHeader* noise = (FrameHeader*)buffer;
	memset(noise, 0, sizeof(FrameHeader));
	noise->type = 2; // NOISE (keeping this as 2 for consistency with server)
}

bool check_for_exit() {
	// Check if user pressed Ctrl+Z or Ctrl+C
	if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {   // Ctrl key is pressed
		if ((GetAsyncKeyState('Z') & 0x8000) || (GetAsyncKeyState('C') & 0x8000)) {  // Z or C key is pressed
			printf("\nExit command detected (Ctrl+Z or Ctrl+C). Shutting down...\n");
			return true;
		}
	}

	// Alternative check for console input
	HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
	DWORD numEvents = 0;

	GetNumberOfConsoleInputEvents(hStdin, &numEvents);
	if (numEvents > 0) {
		INPUT_RECORD inRec[10];  // Read up to 10 events at once
		DWORD numRead;

		ReadConsoleInput(hStdin, inRec, 10, &numRead);
		for (DWORD i = 0; i < numRead; i++) {
			if (inRec[i].EventType == KEY_EVENT &&
				inRec[i].Event.KeyEvent.bKeyDown) {
				// Check for Ctrl+Z or Ctrl+C
				if ((inRec[i].Event.KeyEvent.wVirtualKeyCode == 'Z' ||
					inRec[i].Event.KeyEvent.wVirtualKeyCode == 'C') &&
					(inRec[i].Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
					printf("\nExit command detected (Ctrl+Z or Ctrl+C). Shutting down...\n");
					return true;
				}
			}
		}
	}

	return false;
}
/*
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
}*/

// Broadcast a message to all connected clients
void broadcast_to_all(char* buffer, int length) {
	ClientNode* current = client_list;

	while (current != NULL) {
		if (current->info.active) {
			int sent = send(current->info.socket, buffer, length, 0);
			if (sent == SOCKET_ERROR) {
				int err = WSAGetLastError();
				if (err != WSAEWOULDBLOCK) {
					fprintf(stderr, "Error sending to client: %d\n", err);
					current->info.active = false;
				}
			}
		}
		current = current->next;
	}
}

// Function to print all archived statistics and current client statistics
void print_all_statistics() {
	fprintf(stderr, "\n=== Channel Statistics ===\n");

	// First print statistics for any remaining connected clients
	ClientNode* current = client_list;
	while (current != NULL) {
		if (current->info.total_frames > 0) {  // Report all clients that sent frames
			double bandwidth_mbps = 0.0;
			if (current->info.first_frame_time != 0 &&
				current->info.last_frame_time > current->info.first_frame_time) {

				double duration_sec = (double)(current->info.last_frame_time - current->info.first_frame_time) / CLOCKS_PER_SEC;
				if (duration_sec > 0) {
					bandwidth_mbps = (current->info.total_bytes * 8.0) / (duration_sec * 1000000); // Mbps
				}
			}

			fprintf(stderr, "From %s port %d: %d frames, %d collisions\n",
				inet_ntoa(current->info.addr.sin_addr),
				ntohs(current->info.addr.sin_port),
				current->info.total_frames,
				current->info.collision_count);

			fprintf(stderr, "Average bandwidth: %.3f Mbps\n\n", bandwidth_mbps);
		}
		current = current->next;
	}

	// Then print statistics for all archived servers
	ArchiveNode* archive = archive_list;
	while (archive != NULL) {
		double bandwidth_mbps = 0.0;
		if (archive->info.first_frame_time != 0 &&
			archive->info.last_frame_time > archive->info.first_frame_time) {

			double duration_sec = (double)(archive->info.last_frame_time - archive->info.first_frame_time) / CLOCKS_PER_SEC;
			if (duration_sec > 0) {
				bandwidth_mbps = (archive->info.total_bytes * 8.0) / (duration_sec * 1000000); // Mbps
			}
		}

		fprintf(stderr, "From %s port %d: %d frames, %d collisions\n",
			inet_ntoa(archive->info.addr.sin_addr),
			ntohs(archive->info.addr.sin_port),
			archive->info.total_frames,
			archive->info.collision_count);

		fprintf(stderr, "Average bandwidth: %.3f Mbps\n\n", bandwidth_mbps);

		archive = archive->next;
	}
}

// Function to free the archive list
void cleanup_archive() {
	ArchiveNode* current = archive_list;
	ArchiveNode* next;

	while (current != NULL) {
		next = current->next;
		free(current);
		current = next;
	}

	archive_list = NULL;
}

int main(int argc, char *argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <chan_port> <slot_time_ms>\n", argv[0]);
		return 1;
	}

	int chan_port = atoi(argv[1]);
	int slot_time_ms = atoi(argv[2]);

	// Create the listening socket
	SOCKET tcp_s = create_listening_socket(chan_port);
	if (tcp_s == INVALID_SOCKET) {
		return 1;
	}

	printf("Channel listening on port %d with slot time %d ms\n", chan_port, slot_time_ms);

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

		// Add all active client sockets to the set
		ClientNode* current = client_list;
		while (current != NULL) {
			if (current->info.active) {
				FD_SET(current->info.socket, &readfds);
			}
			current = current->next;
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

				// Add the new client
				ClientNode* new_client = add_client(new_socket, peer_addr);
				if (!new_client) {
					fprintf(stderr, "Failed to add new client, closing connection\n");
					closesocket(new_socket);
				}
			}
		}

		// Arrays to track received frames in this slot
		typedef struct {
			char* buffer;
			int length;
			ClientNode* sender;
		} ReceivedFrame;

		ReceivedFrame* received_frames = (ReceivedFrame*)malloc(client_count * sizeof(ReceivedFrame));
		if (!received_frames) {
			fprintf(stderr, "Memory allocation failed for received frames\n");
			continue;
		}

		int frames_received = 0;

		// Check client sockets for data
		current = client_list;
		while (current != NULL) {
			ClientNode* next = current->next; // Save next pointer in case current gets removed

			if (current->info.active && FD_ISSET(current->info.socket, &readfds)) {
				int bytes = recv(current->info.socket, current->frame_buffer, MAX_FRAME_SIZE, 0);

				if (bytes > 0 && bytes >= sizeof(FrameHeader)) {
					current->frame_length = bytes;

					// Store frame for later processing
					if (frames_received < client_count) {
						received_frames[frames_received].buffer = current->frame_buffer;
						received_frames[frames_received].length = bytes;
						received_frames[frames_received].sender = current;
						frames_received++;
					}

					// Update statistics
					current->info.total_frames++;
					if (current->info.first_frame_time == 0) {
						current->info.first_frame_time = clock();
					}
					current->info.last_frame_time = clock();
					current->info.total_bytes += bytes;

					// Print received message information
					FrameHeader* header = (FrameHeader*)current->frame_buffer;
					printf("Received frame from %s:%d - Type: %d, Seq: %u, Length: %d bytes\n",
						inet_ntoa(current->info.addr.sin_addr),
						ntohs(current->info.addr.sin_port),
						header->type,
						header->seq_num,
						bytes);
				}
				else if (bytes == 0 || (bytes == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
					// Client disconnected or error
					printf("Client disconnected: %s:%d\n",
						inet_ntoa(current->info.addr.sin_addr),
						ntohs(current->info.addr.sin_port));
					// Remove call to print_statistics() - we only want to print at exit
					current->info.active = false;
					remove_client(current->info.socket);
				}
			}

			current = next;
		}

		// Process received frames
		if (frames_received == 1) {
			// No collision - broadcast the frame to all clients
			FrameHeader* header = (FrameHeader*)received_frames[0].buffer;
			printf("Broadcasting frame - Type: %d, Seq: %u, Length: %d bytes\n",
				header->type,
				header->seq_num,
				received_frames[0].length);

			broadcast_to_all(received_frames[0].buffer, received_frames[0].length);
		}
		else if (frames_received > 1) {
			// Collision - send noise and update collision counts
			printf("Collision detected! %d frames received simultaneously\n", frames_received);

			char noise_buffer[sizeof(FrameHeader)];
			create_noise_frame(noise_buffer);
			broadcast_to_all(noise_buffer, sizeof(FrameHeader));

			// Update collision statistics
			for (int k = 0; k < frames_received; k++) {
				received_frames[k].sender->info.collision_count++;
			}
		}

		// Free the received frames array
		free(received_frames);
	}

	// Print statistics after Ctrl+Z
	print_all_statistics();

	// Clean up archives
	cleanup_archive();

	// Clean up and free resources
	cleanup_clients();
	closesocket(tcp_s);
	WSACleanup();

	return 0;
}