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
#include <conio.h>  // Added for _kbhit() and _getch() functions

#pragma comment(lib, "Ws2_32.lib")

#define MAX_FRAME_SIZE 1600  // Enough room for header + payload
#define FRAME_TYPE_DATA 0
#define FRAME_TYPE_NOISE 2

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
static char global_noise_frame[sizeof(FrameHeader)];


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

// Function to archive before removing
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
FrameHeader* create_noise_frame() {
	// Allocate memory for the noise frame
	FrameHeader* noise_frame = (FrameHeader*)malloc(sizeof(FrameHeader));
	if (!noise_frame) {
		fprintf(stderr, "Failed to allocate memory for noise frame\n");
		return NULL;
	}

	// Clear the frame first
	memset(noise_frame, 0, sizeof(FrameHeader));

	// Set fields
	noise_frame->type = 2;  // NOISE type (explicit value)
	noise_frame->seq_num = 0xFFFFFFFF;  // Special value
	noise_frame->length = sizeof(FrameHeader);

	printf("Created noise frame with Type: %d, Seq: %u, Length: %d\n",
		noise_frame->type, noise_frame->seq_num, noise_frame->length);

	return noise_frame;
}
/*
// Create a noise frame to indicate collision
void create_noise_frame(char* buffer) {
	// First clear the entire buffer
	memset(buffer, 0, sizeof(FrameHeader));

	// Then set the frame header fields
	FrameHeader* noise = (FrameHeader*)buffer;

	// Set noise signal type (explicitly cast to ensure correct type)
	noise->type = (uint16_t)FRAME_TYPE_NOISE;

	// For clarity in debugging, set a special sequence number
	noise->seq_num = 0xFFFFFFFF;

	// Set length to header size only
	noise->length = (uint16_t)sizeof(FrameHeader);

	// Print debug info to verify the noise frame was created correctly
	printf("Created noise frame with Type: %d, Seq: %u, Length: %d\n",
		noise->type, noise->seq_num, noise->length);
}*/

// Enhanced broadcast function specifically for sending noise frames
void broadcast_noise_frame(FrameHeader* noise_frame) {
	ClientNode* current = client_list;
	int successful_sends = 0;
	int failed_sends = 0;

	printf("Broadcasting NOISE frame (collision signal) to all clients:\n");

	// Print the first few bytes of the noise frame for debugging
	printf("Noise frame bytes: ");
	unsigned char* bytes = (unsigned char*)noise_frame;
	for (int i = 0; i < 8; i++) {
		printf("%02X ", bytes[i]);
	}
	printf("...\n");

	while (current != NULL) {
		if (current->info.active) {
			int sent = send(current->info.socket, (char*)noise_frame, sizeof(FrameHeader), 0);

			if (sent == SOCKET_ERROR) {
				int err = WSAGetLastError();
				failed_sends++;

				printf("  Failed to send to %s:%d - Error: %d\n",
					inet_ntoa(current->info.addr.sin_addr),
					ntohs(current->info.addr.sin_port),
					err);

				if (err != WSAEWOULDBLOCK) {
					current->info.active = false;
				}
			}
			else if (sent == sizeof(FrameHeader)) {
				successful_sends++;
				printf("  Sent noise frame to %s:%d\n",
					inet_ntoa(current->info.addr.sin_addr),
					ntohs(current->info.addr.sin_port));
			}
			else {
				printf("  Partial send to %s:%d: %d of %d bytes\n",
					inet_ntoa(current->info.addr.sin_addr),
					ntohs(current->info.addr.sin_port),
					sent, (int)sizeof(FrameHeader));
			}
		}
		current = current->next;
	}

	printf("Noise frame broadcast complete: %d successful, %d failed\n",
		successful_sends, failed_sends);
}

// Function to check for user input (Ctrl+Z)
bool check_for_exit() {
	// Check if stdin has input
	if (_kbhit()) {
		int c = _getch();
		// Check for Control+Z (ASCII 26) or Control+C (ASCII 3)
		if (c == 26 || c == 3) {
			printf("\nExit command detected (Ctrl+Z or Ctrl+C). Shutting down...\n");
			return true;
		}
	}
	return false;
}

// Broadcast a message to all connected clients
void broadcast_to_all(char* buffer, int length) {
	ClientNode* current = client_list;
	int successful_sends = 0;

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
			else if (sent == length) {
				successful_sends++;
			}
		}
		current = current->next;
	}

	printf("Broadcast frame to %d active clients\n", successful_sends);
}

// Function to calculate average bandwidth in Mbps
double calculate_bandwidth(int64_t bytes, clock_t start_time, clock_t end_time) {
	if (start_time == 0 || end_time <= start_time) {
		return 0.0;
	}

	double duration_sec = (double)(end_time - start_time) / CLOCKS_PER_SEC;
	if (duration_sec <= 0) {
		return 0.0;
	}

	// Convert bytes to bits and divide by duration in seconds to get bps, then convert to Mbps
	return (bytes * 8.0) / (duration_sec * 1000000);
}

// Function to print all archived statistics and current client statistics
void print_all_statistics() {
	fprintf(stderr, "\n=== Channel Statistics ===\n");

	// First print statistics for any remaining connected clients
	ClientNode* current = client_list;
	while (current != NULL) {
		if (current->info.total_frames > 0) {  // Report all clients that sent frames
			// Calculate bandwidth
			double bandwidth_mbps = calculate_bandwidth(
				current->info.total_bytes,
				current->info.first_frame_time,
				current->info.last_frame_time
			);

			fprintf(stderr, "From %s port %d: %d frames, %d collisions\n",
				inet_ntoa(current->info.addr.sin_addr),
				ntohs(current->info.addr.sin_port),
				current->info.total_frames,
				current->info.collision_count);

			fprintf(stderr, "Average bandwidth: %.3f Mbps\n", bandwidth_mbps);
		}
		current = current->next;
	}

	// Then print statistics for all archived servers
	ArchiveNode* archive = archive_list;
	while (archive != NULL) {
		// Calculate bandwidth
		double bandwidth_mbps = calculate_bandwidth(
			archive->info.total_bytes,
			archive->info.first_frame_time,
			archive->info.last_frame_time
		);

		fprintf(stderr, "From %s port %d: %d frames, %d collisions\n",
			inet_ntoa(archive->info.addr.sin_addr),
			ntohs(archive->info.addr.sin_port),
			archive->info.total_frames,
			archive->info.collision_count);

		fprintf(stderr, "Average bandwidth: %.3f Mbps\n", bandwidth_mbps);

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

	// Create the noise frame once at startup
	FrameHeader* noise_frame = create_noise_frame();
	if (!noise_frame) {
		fprintf(stderr, "Failed to create noise frame, exiting\n");
		closesocket(tcp_s);
		WSACleanup();
		return 1;
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
		if (ready_count == SOCKET_ERROR) {
			fprintf(stderr, "select() failed: %d\n", WSAGetLastError());
			Sleep(100); // Avoid busy waiting in case of persistent error
			continue;
		}

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

		// Allocate array for received frames
		ReceivedFrame* received_frames = NULL;
		if (client_count > 0) {
			received_frames = (ReceivedFrame*)malloc(client_count * sizeof(ReceivedFrame));
			if (!received_frames) {
				fprintf(stderr, "Memory allocation failed for received frames\n");
				continue;
			}

			// Initialize buffer pointers to NULL for safe cleanup
			for (int i = 0; i < client_count; i++) {
				received_frames[i].buffer = NULL;
			}
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
					if (received_frames && frames_received < client_count) {
						received_frames[frames_received].buffer = malloc(bytes);
						if (received_frames[frames_received].buffer) {
							memcpy(received_frames[frames_received].buffer, current->frame_buffer, bytes);
							received_frames[frames_received].length = bytes;
							received_frames[frames_received].sender = current;
							frames_received++;
						}
						else {
							fprintf(stderr, "Failed to allocate memory for frame data\n");
						}
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
			// Collision detected
			printf("COLLISION DETECTED: %d frames received simultaneously\n", frames_received);

			// Use the specialized function to broadcast the noise frame
			broadcast_noise_frame(noise_frame);

			// Update collision statistics
			for (int k = 0; k < frames_received; k++) {
				if (received_frames[k].sender) {
					received_frames[k].sender->info.collision_count++;
					printf("Incremented collision count for %s:%d to %d\n",
						inet_ntoa(received_frames[k].sender->info.addr.sin_addr),
						ntohs(received_frames[k].sender->info.addr.sin_port),
						received_frames[k].sender->info.collision_count);
				}
			}
		}

		// Free the received frames
		if (received_frames) {
			for (int i = 0; i < frames_received; i++) {
				if (received_frames[i].buffer) {
					free(received_frames[i].buffer);
				}
			}
			free(received_frames);
		}
	}

	// Print statistics after Ctrl+Z
	print_all_statistics();

	// Clean up archives
	cleanup_archive();

	// Clean up and free resources
	cleanup_clients();
	free(noise_frame);  // Free the noise frame
	closesocket(tcp_s);
	WSACleanup();

	return 0;
}