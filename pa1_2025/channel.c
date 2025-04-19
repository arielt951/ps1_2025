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
#include <conio.h>  // For _kbhit() and _getch() functions

#pragma comment(lib, "Ws2_32.lib")

// No hard limit on frame size - will be determined by what servers send
#define INITIAL_BUFFER_SIZE 4096  // Initial buffer size, will grow as needed
#define FRAME_TYPE_DATA 0
#define FRAME_TYPE_NOISE 2

#pragma pack(push, 1)
typedef struct {
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
	bool connected;         // Whether the client is currently connected
	char* buffer;           // Dynamically sized buffer
	int buffer_size;        // Current size of the buffer
	int frame_length;       // Length of current frame in buffer
} ClientInfo;

// Linked list node for client management
typedef struct ClientNode {
	ClientInfo info;
	struct ClientNode* next;
} ClientNode;

// Global linked list head
static ClientNode* client_list = NULL;
static int client_count = 0;

// Forward declarations of functions
SOCKET create_listening_socket(int port);
void ensure_buffer_capacity(ClientNode* client, int required_size);
void mark_client_disconnected(ClientNode* client);
ClientNode* add_client(SOCKET socket, struct sockaddr_in addr);
ClientNode* find_client_by_socket(SOCKET socket);
void cleanup_clients(void);
char* create_noise_frame(void);
void broadcast_noise_frame(char* noise_buffer);
bool check_for_exit(void);
void broadcast_to_all(char* buffer, int length);
double calculate_bandwidth(int64_t bytes, clock_t start_time, clock_t end_time);
void print_all_statistics(void);

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

// Function to ensure a client's buffer is large enough
void ensure_buffer_capacity(ClientNode* client, int required_size) {
	if (!client || required_size <= 0) return;

	if (client->info.buffer_size < required_size) {
		// Need to resize buffer
		char* new_buffer = realloc(client->info.buffer, required_size);
		if (new_buffer) {
			client->info.buffer = new_buffer;
			client->info.buffer_size = required_size;
	//		fprintf(stderr, "Resized buffer for client %s:%d to %d bytes\n",
		//		inet_ntoa(client->info.addr.sin_addr),
		//		ntohs(client->info.addr.sin_port),
			//	required_size);
		}
		else {
	//		fprintf(stderr, "Failed to resize buffer for client %s:%d\n",
	//			inet_ntoa(client->info.addr.sin_addr),
	//			ntohs(client->info.addr.sin_port));
		}
	}
}

// Mark a client as disconnected but keep it in the list for statistics
void mark_client_disconnected(ClientNode* client) {
	if (client && client->info.active) {
		client->info.active = false;
		client->info.connected = false;

	//	fprintf(stderr, "Server %s:%d disconnected (stats will be kept until exit)\n",
	//		inet_ntoa(client->info.addr.sin_addr),
	//		ntohs(client->info.addr.sin_port));

		// Close the socket but keep the client node in the list
		closesocket(client->info.socket);
	}
}

// Create a new client node and add it to the linked list
ClientNode* add_client(SOCKET socket, struct sockaddr_in addr) {
	// Create a new client node
	ClientNode* new_client = (ClientNode*)malloc(sizeof(ClientNode));
	if (!new_client) {
		fprintf(stderr, "Memory allocation failed for client node\n");
		return NULL;
	}

	// Allocate initial buffer
	new_client->info.buffer = (char*)malloc(INITIAL_BUFFER_SIZE);
	if (!new_client->info.buffer) {
		fprintf(stderr, "Memory allocation failed for client buffer\n");
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
	new_client->info.connected = true;
	new_client->info.buffer_size = INITIAL_BUFFER_SIZE;
	new_client->info.frame_length = 0;

	// Add to the beginning of the list (O(1) operation)
	new_client->next = client_list;
	client_list = new_client;
	client_count++;

	fprintf(stderr, "New server connected from %s:%d\n",
		inet_ntoa(addr.sin_addr),
		ntohs(addr.sin_port));

	return new_client;
}

// Find a client by socket
ClientNode* find_client_by_socket(SOCKET socket) {
	ClientNode* current = client_list;

	while (current != NULL) {
		if (current->info.socket == socket) {
			return current;
		}
		current = current->next;
	}

	return NULL;
}

// Cleanup all clients and free resources at the end
void cleanup_clients(void) {
	ClientNode* current = client_list;
	ClientNode* next;

	while (current != NULL) {
		next = current->next;

		if (current->info.connected) {
			closesocket(current->info.socket);
		}

		free(current->info.buffer);
		free(current);

		current = next;
	}

	client_list = NULL;
	client_count = 0;
}

// Create a noise frame to indicate collision
char* create_noise_frame(void) {
	// Allocate memory for the noise frame
	char* noise_buffer = (char*)malloc(sizeof(FrameHeader));
	if (!noise_buffer) {
		fprintf(stderr, "Failed to allocate memory for noise frame\n");
		return NULL;
	}

	// Clear the frame first
	memset(noise_buffer, 0, sizeof(FrameHeader));

	// Set fields
	FrameHeader* noise = (FrameHeader*)noise_buffer;
	noise->type = FRAME_TYPE_NOISE;
	noise->seq_num = 0xFFFFFFFF;  // Special value
	noise->length = sizeof(FrameHeader);

//	fprintf(stderr, "Created noise frame with Type: %d, Seq: %u, Length: %d\n",
//		noise->type, noise->seq_num, noise->length);

	return noise_buffer;
}

// Enhanced broadcast function specifically for sending noise frames
void broadcast_noise_frame(char* noise_buffer) {
	ClientNode* current = client_list;
	int successful_sends = 0;
	int failed_sends = 0;

	fprintf(stderr, "Broadcasting NOISE frame (collision signal) to all clients:\n");

	// Print the first few bytes of the noise frame for debugging
	//fprintf(stderr, "Noise frame bytes: ");
	unsigned char* bytes = (unsigned char*)noise_buffer;
	//for (int i = 0; i < 8; i++) {
	//	fprintf(stderr, "%02X ", bytes[i]);
//	}
	//fprintf(stderr, "...\n");

	while (current != NULL) {
		if (current->info.active) {
			int sent = send(current->info.socket, noise_buffer, sizeof(FrameHeader), 0);

			if (sent == SOCKET_ERROR) {
				int err = WSAGetLastError();
				failed_sends++;

				fprintf(stderr, "  Failed to send to %s:%d - Error: %d\n",
					inet_ntoa(current->info.addr.sin_addr),
					ntohs(current->info.addr.sin_port),
					err);

				if (err != WSAEWOULDBLOCK) {
					current->info.active = false;
				}
			}
			else if (sent == sizeof(FrameHeader)) {
				successful_sends++;
	//			fprintf(stderr, "  Sent noise frame to %s:%d\n",
				//	inet_ntoa(current->info.addr.sin_addr),
				//	ntohs(current->info.addr.sin_port));
			}
			else {
	//			fprintf(stderr, "  Partial send to %s:%d: %d of %d bytes\n",
				//	inet_ntoa(current->info.addr.sin_addr),
				//	ntohs(current->info.addr.sin_port),
				//	sent, (int)sizeof(FrameHeader));
			}
		}
		current = current->next;
	}

//	fprintf(stderr, "Noise frame broadcast complete: %d successful, %d failed\n",
	//	successful_sends, failed_sends);
}

// Function to check for user input (Ctrl+Z)
bool check_for_exit(void) {
	// Check if stdin has input
	if (_kbhit()) {
		int c = _getch();
		// Check for Control+Z (ASCII 26) or Control+C (ASCII 3)
		if (c == 26 || c == 3) {
			fprintf(stderr, "\nExit command detected (Ctrl+Z or Ctrl+C). Shutting down...\n");
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
		//			fprintf(stderr, "Error sending to client: %d\n", err);
					current->info.active = false;
				}
			}
			else if (sent == length) {
				successful_sends++;
			}
		}
		current = current->next;
	}

	//fprintf(stderr, "Broadcast frame to %d active clients\n", successful_sends);
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

// Function to print all statistics
void print_all_statistics(void) {
//	fprintf(stderr, "\n=== Channel Statistics ===\n");

	// Print statistics for all clients (connected and disconnected)
	ClientNode* current = client_list;
	while (current != NULL) {
		if (current->info.total_frames > 0) {  // Only report clients that sent frames
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
	char* noise_buffer = create_noise_frame();
	if (!noise_buffer) {
	//	fprintf(stderr, "Failed to create noise frame, exiting\n");
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
				// Check how many bytes are available
				u_long bytes_available = 0;
				if (ioctlsocket(current->info.socket, FIONREAD, &bytes_available) == 0 && bytes_available > 0) {
					// Ensure buffer is large enough
					ensure_buffer_capacity(current, bytes_available);
				}

				int bytes = recv(current->info.socket, current->info.buffer, current->info.buffer_size, 0);

				if (bytes > 0 && bytes >= sizeof(FrameHeader)) {
					current->info.frame_length = bytes;

					// Store frame for later processing
					if (received_frames && frames_received < client_count) {
						received_frames[frames_received].buffer = malloc(bytes);
						if (received_frames[frames_received].buffer) {
							memcpy(received_frames[frames_received].buffer, current->info.buffer, bytes);
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
					FrameHeader* header = (FrameHeader*)current->info.buffer;
		/*			printf("Received frame from %s:%d - Type: %d, Seq: %u, Length: %d bytes\n",
						inet_ntoa(current->info.addr.sin_addr),
						ntohs(current->info.addr.sin_port),
						header->type,
						header->seq_num,
						bytes);*/
				}
				else if (bytes == 0 || (bytes == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
					// Client disconnected or error
					printf("Client disconnected: %s:%d\n",
						inet_ntoa(current->info.addr.sin_addr),
						ntohs(current->info.addr.sin_port));
						
					// Mark as disconnected but keep in list
					mark_client_disconnected(current);
				}
			}

			current = next;
		}

		// Process received frames
		if (frames_received == 1) {
			// No collision - broadcast the frame to all clients
			FrameHeader* header = (FrameHeader*)received_frames[0].buffer;
	/*		printf("Broadcasting frame - Type: %d, Seq: %u, Length: %d bytes\n",
				header->type,
				header->seq_num,
				received_frames[0].length);
				*/
			broadcast_to_all(received_frames[0].buffer, received_frames[0].length);
		}
		else if (frames_received > 1) {
			// Collision detected
			printf("COLLISION DETECTED: %d frames received simultaneously\n", frames_received);

			// Use the specialized function to broadcast the noise frame
			broadcast_noise_frame(noise_buffer);

			// Update collision statistics
			for (int k = 0; k < frames_received; k++) {
				if (received_frames[k].sender) {
					received_frames[k].sender->info.collision_count++;
		/*			printf("Incremented collision count for %s:%d to %d\n",
						inet_ntoa(received_frames[k].sender->info.addr.sin_addr),
						ntohs(received_frames[k].sender->info.addr.sin_port),
						received_frames[k].sender->info.collision_count);*/
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

	// Clean up and free resources
	cleanup_clients();
	free(noise_buffer);  // Free the noise frame buffer
	closesocket(tcp_s);
	WSACleanup();

	return 0;
}