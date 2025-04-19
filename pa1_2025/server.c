#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <winsock2.h>
#include <time.h>
#include <stdbool.h>
#include <conio.h>  // For _kbhit() and _getch() functions

#pragma comment(lib, "Ws2_32.lib")

#define MAX_ATTEMPTS 10
#define FRAME_TYPE_DATA 0
#define FRAME_TYPE_NOISE 2
#define CONNECTION_RETRY_MS 1000  // Time between connection retry attempts
#define MAX_CTRL_Z_WAIT_SEC 60     // Maximum time to wait for Ctrl+Z input in seconds

#pragma pack(push, 1)
typedef struct {
	uint8_t src_mac[6];
	uint8_t dst_mac[6];
	uint16_t type;        // 0 = data, 2 = noise
	uint32_t seq_num;
	uint16_t length;
} FrameHeader;
#pragma pack(pop)

// Function prototypes
bool check_for_exit(void);
SOCKET connect_to_channel(const char *chan_ip, int chan_port, int timeout_sec);
void flush_socket(SOCKET s);
int is_same_frame_header(FrameHeader* sent_header, FrameHeader* recv_header);

// Function to check for Ctrl+Z input from user
bool check_for_exit(void) {
	if (_kbhit()) {
		int c = _getch();
		// Check for Control+Z (ASCII 26) or Control+C (ASCII 3)
		if (c == 26 || c == 3) {
			//fprintf(stderr, "\nExit command detected (Ctrl+Z or Ctrl+C). Exiting...\n");
			return true;
		}
	}
	return false;
}

// Function to connect to channel with retry mechanism
SOCKET connect_to_channel(const char *chan_ip, int chan_port, int timeout_sec) {
	WSADATA wsaData;
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		fprintf(stderr, "Error at WSAStartup()\n");
		return INVALID_SOCKET;
	}

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		fprintf(stderr, "Error at socket(): %ld\n", WSAGetLastError());
		WSACleanup();
		return INVALID_SOCKET;
	}

	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(chan_port);
	server_addr.sin_addr.s_addr = inet_addr(chan_ip);

	// Keep trying to connect until timeout
	//fprintf(stderr, "Attempting to connect to channel at %s:%d\n", chan_ip, chan_port);
	//fprintf(stderr, "Press Ctrl+Z to cancel and exit...\n");

	clock_t start_time = clock();
	clock_t current_time;
	bool connected = false;

	do {
		// Check if user wants to exit
		if (check_for_exit()) {
			closesocket(s);
			WSACleanup();
			exit(0); // Exit the program if user presses Ctrl+Z
		}

		if (connect(s, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
			connected = true;
			fprintf(stderr, "Successfully connected to channel\n");
			break;
		}

		int error = WSAGetLastError();
		if (error != WSAECONNREFUSED && error != WSAENETUNREACH && error != WSAETIMEDOUT) {
			fprintf(stderr, "Connection error: %d\n", error);
			break;
		}

		fprintf(stderr, "Channel not available yet, retrying in %d ms...\n", CONNECTION_RETRY_MS);
		Sleep(CONNECTION_RETRY_MS);

		current_time = clock();
	} while (((current_time - start_time) / CLOCKS_PER_SEC) < timeout_sec);

	if (!connected) {
		fprintf(stderr, "Connection attempts timed out after %d seconds\n", timeout_sec);
		closesocket(s);
		WSACleanup();
		return INVALID_SOCKET;
	}

	return s;
}

// Function to flush all pending data from a socket
void flush_socket(SOCKET s) {
	u_long avail = 0;

	// Check how many bytes are available to read
	if (ioctlsocket(s, FIONREAD, &avail) != 0) {
	//	fprintf(stderr, "Error checking socket buffer: %d\n", WSAGetLastError());
		return;
	}

	if (avail > 0) {
	//	fprintf(stderr, "Flushing %lu bytes from socket\n", avail);

		char temp_buf[1024];
		while (avail > 0) {
			int to_read = (avail > sizeof(temp_buf)) ? sizeof(temp_buf) : avail;
			int read = recv(s, temp_buf, to_read, 0);

			if (read <= 0) {
				if (read < 0) {
					fprintf(stderr, "Error during socket flush: %d\n", WSAGetLastError());
				}
				break;
			}

			avail -= read;
		}

	//	fprintf(stderr, "Socket flushed successfully\n");
	}
}

// Function to verify if received frame header matches sent frame header
int is_same_frame_header(FrameHeader* sent_header, FrameHeader* recv_header) {
	// Debug print to see what headers we're comparing
	/*fprintf(stderr, "Comparing headers - Sent: type=%d, seq=%u, len=%d vs Received: type=%d, seq=%u, len=%d\n",
		sent_header->type, sent_header->seq_num, sent_header->length,
		recv_header->type, recv_header->seq_num, recv_header->length);
		*/
	// Check if all header fields match
	if (recv_header->type != sent_header->type ||
		recv_header->seq_num != sent_header->seq_num ||
		recv_header->length != sent_header->length ||
		memcmp(recv_header->src_mac, sent_header->src_mac, 6) != 0) {
		return 0;  // Headers don't match
	}

	return 1;  // Headers match
}

int main(int argc, char *argv[]) {
	if (argc != 8) {
		fprintf(stderr, "Usage: %s <chan_ip> <chan_port> <file_name> <frame_size> <slot_time> <seed> <timeout>\n", argv[0]);
		return 1;
	}

	// Parse arguments 
	const char *chan_ip = argv[1];
	int chan_port = atoi(argv[2]);
	const char *file_name = argv[3];
	int frame_size = atoi(argv[4]);
	int slot_time_ms = atoi(argv[5]);
	int seed = atoi(argv[6]);
	int timeout_sec = atoi(argv[7]);

	// Ensure frame size is valid, only constraint is it must be larger than header size
	if (frame_size <= sizeof(FrameHeader)) {
		fprintf(stderr, "Error: Frame size must be larger than header size (%d bytes)\n", (int)sizeof(FrameHeader));
		return 1;
	}

	//fprintf(stderr, "Using frame size: %d bytes\n", frame_size);

	srand(seed);

	// Connect to the channel with retry mechanism
	SOCKET s = connect_to_channel(chan_ip, chan_port, timeout_sec);
	if (s == INVALID_SOCKET) {
		fprintf(stderr, "Error: Failed to connect to channel at %s:%d\n", chan_ip, chan_port);
		return 1;
	}

	FILE *fp = fopen(file_name, "rb"); //open the file 
	if (!fp) {
	//	perror("Error opening file");
		closesocket(s);
		WSACleanup();
		return 1;
	}

	// Retrieve file size 
	fseek(fp, 0, SEEK_END);
	int total_file_size = ftell(fp);
	rewind(fp);

	// Calculate the number of frames to send in total
	const int header_size = sizeof(FrameHeader);
	const int payload_size = frame_size - header_size;
	const int total_frames = (total_file_size + payload_size - 1) / payload_size;

	fprintf(stderr, "Starting transmission of %s (%d bytes in %d frames)\n",
		file_name, total_file_size, total_frames);
	fprintf(stderr, "Frame size: %d bytes (header: %d bytes, payload: %d bytes)\n",
		frame_size, header_size, payload_size);

	uint8_t my_mac[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01 };
	uint8_t channel_mac[6] = { 0xFF, 0xEE, 0xDD, 0x00, 0x00, 0x00 };

	// Dynamically allocate memory for frame and receive buffer based on frame_size
	char *frame = malloc(frame_size);
	if (!frame) {
		fprintf(stderr, "Memory allocation failed for frame buffer\n");
		fclose(fp);
		closesocket(s);
		WSACleanup();
		return 1;
	}

	char *recv_buffer = malloc(frame_size);
	if (!recv_buffer) {
		fprintf(stderr, "Memory allocation failed for receive buffer\n");
		free(frame);
		fclose(fp);
		closesocket(s);
		WSACleanup();
		return 1;
	}

	int total_transmissions = 0;
	int max_transmissions = 0;
	int successful_frames = 0;

	clock_t start_time = clock();

	for (int frame_idx = 0; frame_idx < total_frames; ++frame_idx) {
		int current_attempt = 0;
		int success = 0;

		// Calculate frame payload size (handle last frame)
		int frame_payload_size = payload_size;
		if (frame_idx == total_frames - 1) {
			int remaining_bytes = total_file_size - (frame_idx * payload_size);
			if (remaining_bytes < payload_size)
				frame_payload_size = remaining_bytes;
		}

		// Build header for this frame
		FrameHeader header;
		memcpy(header.src_mac, my_mac, 6);
		memcpy(header.dst_mac, channel_mac, 6);
		header.type = FRAME_TYPE_DATA;
		header.seq_num = frame_idx;
		header.length = frame_payload_size;

		// Copy header into frame buffer
		memcpy(frame, &header, header_size);

		// Read the file data for this frame
		fseek(fp, frame_idx * payload_size, SEEK_SET);
		int bytes_read = fread(frame + header_size, 1, frame_payload_size, fp);
		if (bytes_read != frame_payload_size) {
			fprintf(stderr, "Error reading file at frame %d\n", frame_idx);
			break;
		}

		// Transmission loop for this frame - keep trying until success or MAX_ATTEMPTS
		while (current_attempt < MAX_ATTEMPTS && !success) {
			current_attempt++;
			total_transmissions++;

			// Clear receive buffer before sending
			memset(recv_buffer, 0, frame_size);

			// Flush socket to ensure clean state before sending
			flush_socket(s);

			// Send the frame
			int bytes_sent = send(s, frame, header_size + frame_payload_size, 0);
			if (bytes_sent != header_size + frame_payload_size) {
				fprintf(stderr, "Error sending frame %d (attempt %d)\n", frame_idx, current_attempt);
				continue;
			}

		//	fprintf(stderr, "Sent frame %d (attempt %d) - %d bytes\n",
		//		frame_idx, current_attempt, header_size + frame_payload_size);

			// Set up for listening with timeout
			fd_set readfds;
			FD_ZERO(&readfds);
			FD_SET(s, &readfds);

			struct timeval timeout;
			timeout.tv_sec = timeout_sec;
			timeout.tv_usec = 0;

			// Wait for response with timeout
			int select_result = select(s + 1, &readfds, NULL, NULL, &timeout);

			if (select_result > 0) {
				// Data available to read
				int bytes_recv = recv(s, recv_buffer, frame_size, 0);
				//fprintf(stderr, "Received %d bytes back\n", bytes_recv);

				if (bytes_recv < header_size) {
				//	fprintf(stderr, "Received truncated frame or noise signal\n");
					// Flush socket and continue to backoff logic
					flush_socket(s);
				}
				else {
					FrameHeader* response = (FrameHeader*)recv_buffer;

					if (response->type == FRAME_TYPE_NOISE) {
						fprintf(stderr, "Collision detected (noise frame type=%d)\n", response->type);
						// Flush socket to clear any buffered data after collision
						flush_socket(s);
					}
					else if (is_same_frame_header((FrameHeader*)frame, response)) {
						// SUCCESS - Header matches between sent and received frame
						success = 1;
						successful_frames++;
						//fprintf(stderr, "Frame %d transmitted successfully\n", frame_idx);
						break;  // Exit retry loop
					}
					else {
						// Received a frame but the header doesn't match ours
					//	fprintf(stderr, "Received mismatched frame, type=%d, seq=%u\n",
					//		response->type, response->seq_num);
						// Flush socket and continue to backoff logic
						flush_socket(s);
					}
				}
			}
			else if (select_result == 0) {
				// Timeout occurred, treat as collision
				fprintf(stderr, "Timeout waiting for frame %d (attempt %d)\n", frame_idx, current_attempt);
				// No need to flush after timeout as nothing was received
			}
			else {
				// Select error
				fprintf(stderr, "Error in select(): %d\n", WSAGetLastError());
				// Flush socket just to be sure
				flush_socket(s);
			}

			// Only continue with backoff if we haven't succeeded
			if (success) {
				break;  // Skip backoff logic if successful
			}

			// Check for max attempts
			if (current_attempt >= MAX_ATTEMPTS) {
				fprintf(stderr, "Max attempts reached for frame %d\n", frame_idx);
				break;
			}

			// Exponential backoff
			int backoff_range = 1 << current_attempt;  // 2^k
			int rand_slots = rand() % backoff_range;
			int backoff_time = rand_slots * slot_time_ms;

			fprintf(stderr, "Backoff: waiting %d ms before retrying frame %d\n",
				backoff_time, frame_idx);
			Sleep(backoff_time);
		}

		// Check if this frame failed after all attempts
		if (!success) {
			fprintf(stderr, "Frame %d failed after %d attempts\n", frame_idx, MAX_ATTEMPTS);
			break;  // Exit the main frame loop
		}

		// Update max transmissions stat
		if (current_attempt > max_transmissions)
			max_transmissions = current_attempt;
	}

	// Calculate statistics
	clock_t end_time = clock();
	int duration_ms = (int)((end_time - start_time) * 1000.0 / CLOCKS_PER_SEC);
	double avg_transmissions = (double)total_transmissions / (successful_frames > 0 ? successful_frames : 1);
	double avg_bandwidth_mbps = successful_frames > 0 ?
		(8.0 * total_file_size) / (duration_ms / 1000.0) / 1000000.0 : 0;

	// Print results to stderr as required
	fprintf(stderr, "\n");
	fprintf(stderr, "Sent file %s\n", file_name);
	fprintf(stderr, "Result: %s\n", (successful_frames == total_frames) ? "Success :)" : "Failure :(");
	fprintf(stderr, "File size: %d Bytes (%d frames)\n", total_file_size, total_frames);
	fprintf(stderr, "Total transfer time: %d milliseconds\n", duration_ms);
	fprintf(stderr, "Transmissions/frame: average %.2f, maximum %d\n", avg_transmissions, max_transmissions);
	fprintf(stderr, "Average bandwidth: %.3f Mbps\n", avg_bandwidth_mbps);

	// Clean up
	free(frame);
	free(recv_buffer);
	fclose(fp);
	closesocket(s);
	WSACleanup();
	return 0;
}