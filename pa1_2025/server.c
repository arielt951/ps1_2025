// server.c
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <winsock2.h>
#include <time.h>

#pragma comment(lib, "Ws2_32.lib")

#define MAX_ATTEMPTS 10

#pragma pack(push, 1)
typedef struct {
	uint8_t src_mac[6];   // Fake MAC address of sender
	uint8_t dst_mac[6];   // Fake MAC address of receiver (or broadcast)
	uint16_t type;        // 0 = data, 2 = noise
	uint32_t seq_num;
	uint16_t length;
} FrameHeader;
#pragma pack(pop)

SOCKET connect_to_channel(const char *chan_ip, int chan_port) { //function to connect to the socket
	// Initialize Winsock
	WSADATA wsaData;
	//start wsa
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("Error at WSAStartup()\n");
		return INVALID_SOCKET;
	}

	// Create socket to connect to the channel
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		printf("Error at socket(): %ld\n", WSAGetLastError());
		WSACleanup();
		return INVALID_SOCKET;
	}

	// Setup destination address struct
	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET; // IPv4
	server_addr.sin_port = htons(chan_port); // Connect to the port as given
	server_addr.sin_addr.s_addr = inet_addr(chan_ip); // IP to connect to

	// Connect to the channel
	if (connect(s, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		fprintf(stderr, "Connection to channel failed\n");
		closesocket(s);
		WSACleanup();
		return INVALID_SOCKET;
	}

	return s;
}

int main(int argc, char *argv[]) {
	if (argc != 8) {
		fprintf(stderr, "Usage: %s <chan_ip> <chan_port> <file_name> <frame_size> <slot_time> <seed> <timeout>\n", argv[0]);
		return 1;
	}

	// Parse command-line arguments
	const char *chan_ip = argv[1];
	int chan_port = atoi(argv[2]);
	const char *file_name = argv[3];  // used for printing/logging only
	int frame_size = atoi(argv[4]);
	int slot_time_ms = atoi(argv[5]);  // in milliseconds
	int seed = atoi(argv[6]);
	int timeout_sec = atoi(argv[7]);

	srand(seed);  // Seed for random backoff

	SOCKET s = connect_to_channel(chan_ip, chan_port); //connect to socket
	fprintf(stderr, "Error: Failed to connect to channel at %s:%d\n", chan_ip, chan_port);
	if (s == INVALID_SOCKET) {
		return 1;
	}

	const int total_file_size = 768000;
	const int header_size = sizeof(FrameHeader);
	const int payload_size = frame_size - header_size;
	const int total_frames = (total_file_size + payload_size - 1) / payload_size;

	// MAC addresses (fictonal)
	uint8_t my_mac[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01 };       // This server's MAC
	uint8_t channel_mac[6] = { 0xFF, 0xEE, 0xDD, 0x00, 0x00, 0x00 };   // Channel MAC

	char *frame = malloc(frame_size);
	char recv_buffer[1600];
	int total_transmissions = 0;
	int max_transmissions = 0;
	int successful_frames = 0;

	clock_t start_time = clock();

	for (int i = 0; i < total_frames; ++i) { //count for the number of frames
		int attempts = 0;
		int success = 0;
		// Calculate proper payload size for the last frame
		int frame_payload_size = payload_size;
		if (i == total_frames - 1) {
			// For the last frame, calculate remaining bytes
			int remaining_bytes = total_file_size - (i * payload_size);
			if (remaining_bytes < payload_size) {
				frame_payload_size = remaining_bytes;
			}
		}

		FrameHeader header;
		memcpy(header.src_mac, my_mac, 6);
		memcpy(header.dst_mac, channel_mac, 6);
		header.type = 0;
		header.seq_num = i;
		header.length = payload_size;

		memcpy(frame, &header, sizeof(FrameHeader));
		// Payload is not meaningful, can be left uninitialized

		while (attempts < MAX_ATTEMPTS) { //try to send and listen to the echo

			// Send frame
			send(s, frame, sizeof(FrameHeader) + frame_payload_size, 0);
			++attempts;
			++total_transmissions;
			printf("Frame %d sent | Attempt %d | Total transmissions: %d\n", i, attempts, total_transmissions);

			// Wait for response using select()
			fd_set readfds;
			FD_ZERO(&readfds);
			FD_SET(s, &readfds);

			struct timeval timeout;
			timeout.tv_sec = timeout_sec;
			timeout.tv_usec = 0;

			int ready = select(0, &readfds, NULL, NULL, &timeout);
			if (ready > 0) {
				int bytes = recv(s, recv_buffer, sizeof(recv_buffer), 0);
				if (bytes >= sizeof(FrameHeader)) {
					FrameHeader *response = (FrameHeader *)recv_buffer;

					// Compare the sent header and response header byte-by-byte
					if (memcmp(&header, response, sizeof(FrameHeader)) == 0) {
						success = 1;
						successful_frames++;
						break;
					}
					else {
						printf("response frame does not match (collision or noise)\n");
					}
				}
			}

			// If collision or timeout: backoff
			int backoff_range = 1 << attempts;
			int n = rand() % backoff_range;
			Sleep(n * slot_time_ms);  // Windows sleep in milliseconds
		}

		if (!success) {
			fprintf(stderr, "Frame %d failed after %d attempts\n", i, MAX_ATTEMPTS);
		}

		if (attempts > max_transmissions)
			max_transmissions = attempts;
	}

	clock_t end_time = clock(); //measure time
	int duration_ms = (int)((end_time - start_time) * 1000 / CLOCKS_PER_SEC);
	double avg_transmissions = (double)total_transmissions / total_frames;
	double avg_bandwidth_mbps = (8.0 * total_file_size) / (duration_ms * 1000.0);  // bits/ms = Mbps

	// Print summary
	fprintf(stderr, "Sent file %s\n", file_name);
	fprintf(stderr, "Result: %s\n", (successful_frames == total_frames) ? "Success :)" : "Failure :(");
	fprintf(stderr, "File size: %d Bytes (%d frames)\n", total_file_size, total_frames);
	fprintf(stderr, "Total transfer time: %d milliseconds\n", duration_ms);
	fprintf(stderr, "Transmissions/frame: average %.2f, maximum %d\n", avg_transmissions, max_transmissions);
	fprintf(stderr, "Average bandwidth: %.3f Mbps\n", avg_bandwidth_mbps);

	// Cleanup
	free(frame);
	closesocket(s);
	WSACleanup();
	return 0;

}

