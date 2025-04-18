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
	uint8_t src_mac[6];
	uint8_t dst_mac[6];
	uint16_t type;        // 0 = data, 2 = noise
	uint32_t seq_num;
	uint16_t length;
} FrameHeader;
//20bytes header size
#pragma pack(pop)

//function to open and connect to socket
SOCKET connect_to_channel(const char *chan_ip, int chan_port) { 
	WSADATA wsaData; //start 
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("Error at WSAStartup()\n");
		return INVALID_SOCKET;
	}

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); //create socket
	if (s == INVALID_SOCKET) {
		printf("Error at socket(): %ld\n", WSAGetLastError());
		WSACleanup();
		return INVALID_SOCKET;
	}

	struct sockaddr_in server_addr; //configure addr
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(chan_port);
	server_addr.sin_addr.s_addr = inet_addr(chan_ip);

	if (connect(s, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) { //connect
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
	//Parse agruments 
	const char *chan_ip = argv[1];
	int chan_port = atoi(argv[2]);
	const char *file_name = argv[3];
	int frame_size = atoi(argv[4]);
	int slot_time_ms = atoi(argv[5]);
	int seed = atoi(argv[6]);
	int timeout_sec = atoi(argv[7]);

	srand(seed);
	
	SOCKET s = connect_to_channel(chan_ip, chan_port);
	if (s == INVALID_SOCKET) {
		fprintf(stderr, "Error: Failed to connect to channel at %s:%d\n", chan_ip, chan_port);
		return 1;
	}
	
	FILE *fp = fopen(file_name, "rb"); //open the file 
	if (!fp) {
		perror("Error opening file");
		closesocket(s);
		WSACleanup();
		return 1;
	}
	//retrieve file size 
	fseek(fp, 0, SEEK_END);
	int total_file_size = ftell(fp);
	rewind(fp);

	//calculating the # of frames to send in total
	const int header_size = sizeof(FrameHeader);
	const int payload_size = frame_size - header_size;
	const int total_frames = (total_file_size + payload_size - 1) / payload_size;

	uint8_t my_mac[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01 };
	uint8_t channel_mac[6] = { 0xFF, 0xEE, 0xDD, 0x00, 0x00, 0x00 };

	//allocating memory for each frame 
	char *frame = malloc(frame_size);
	char recv_buffer[1600];
	int total_transmissions = 0;
	int max_transmissions = 0;
	int successful_frames = 0;

	clock_t start_time = clock();

	for (int i = 0; i < total_frames; ++i) { //frame ammout loop
		int attempts = 0;
		int success = 0;

		//last frame may no be in the full size
		int frame_payload_size = payload_size;
		if (i == total_frames - 1) {
			int remaining_bytes = total_file_size - (i * payload_size);
			if (remaining_bytes < payload_size)
				frame_payload_size = remaining_bytes;
		}

		//build headers for each frame
		FrameHeader header;
		memcpy(header.src_mac, my_mac, 6);
		memcpy(header.dst_mac, channel_mac, 6);
		header.type = 0;
		header.seq_num = i;
		header.length = frame_payload_size;

		memcpy(frame, &header, header_size);

		int bytes_read = fread(frame + header_size, 1, frame_payload_size, fp);
		if (bytes_read != frame_payload_size) {
			fprintf(stderr, "Error reading file at frame %d\n", i);
			break;
		}

		
		//send process 
		while (attempts < MAX_ATTEMPTS) {
			send(s, frame, header_size + frame_payload_size, 0);
			++attempts;
			++total_transmissions;
			printf("Frame %d sent | Attempt %d | Total transmissions: %d\n", i, attempts, total_transmissions);

			//listening to the socket a after send
			fd_set readfds;
			FD_ZERO(&readfds);
			FD_SET(s, &readfds);

			//listening the timeout time
			struct timeval timeout;
			timeout.tv_sec = timeout_sec;
			timeout.tv_usec = 0;
			int ready = select(0, &readfds, NULL, NULL, &timeout);

			if (ready > 0) { //data recieved 
				int bytes = recv(s, recv_buffer, sizeof(recv_buffer), 0); 
				if (bytes >= header_size) { //check if at least header was recieved
					FrameHeader *response = (FrameHeader *)recv_buffer;
					if (response->type == 2) {
						printf("Received noise frame (collision detected)\n");
						// continue to backoff and retry
					}
					else if (memcmp(&header, response, header_size) == 0) {
						success = 1;
						successful_frames++;
						printf("massage number %d echoed succesfully\n", i);
						break; //if data recv matches the sent go to the next frame
					}
				}
			}
			printf("Entering backoff proc after frame #%d\n", i);
			//exp backoff waiting time 
			int backoff_range = 1 << attempts; //2^k
			int n = rand() % backoff_range;
			Sleep(n * slot_time_ms);


		}

		if (!success) {
			fprintf(stderr, "Frame %d failed after %d attempts\n", i, MAX_ATTEMPTS);
			break;
		}

		if (attempts > max_transmissions)
			max_transmissions = attempts;
	}

	clock_t end_time = clock();
	int duration_ms = (int)((end_time - start_time) * 1000 / CLOCKS_PER_SEC);
	double avg_transmissions = (double)total_transmissions / total_frames;
	double avg_bandwidth_mbps = (8.0 * total_file_size) / (duration_ms * 1000.0);

	fprintf(stderr, "Sent file %s\n", file_name);
	fprintf(stderr, "Result: %s\n", (successful_frames == total_frames) ? "Success :)" : "Failure :(");
	fprintf(stderr, "File size: %d Bytes (%d frames)\n", total_file_size, total_frames);
	fprintf(stderr, "Total transfer time: %d milliseconds\n", duration_ms);
	fprintf(stderr, "Transmissions/frame: average %.2f, maximum %d\n", avg_transmissions, max_transmissions);
	fprintf(stderr, "Average bandwidth: %.3f Mbps\n", avg_bandwidth_mbps);

	free(frame);
	fclose(fp);
	closesocket(s);
	WSACleanup();
	return 0;
}
