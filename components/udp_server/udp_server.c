#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "udp_server.h"
#include "storage.h"
#include "app_config.h"
#include "led_driver.h"
#include <string.h>

// CRC8 implementation (e.g., Dallas/Maxim)
static uint8_t crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
    }
    return crc;
}

// Protocol definitions
#define GT_HEADER_0 'G'
#define GT_HEADER_1 'T'

// Protocol commands
typedef enum {
    CMD_DISCOVERY = 0,
    CMD_GET_CONFIG = 1,
    CMD_SET_CONFIG = 2,
    CMD_SET_LEDS = 6,
    CMD_STREAM_FRAME = 7
} protocol_cmd_t;

// Controller modes
typedef enum {
    MODE_IDLE,
    MODE_STATIC,
    MODE_STREAMING
} controller_mode_t;

#define UDP_PORT 1234
#define TAG "UDP_SERVER"

// --- Chunk assembly state ---
// Buffer to store LED IDs from chunks
static uint16_t led_id_buffer[MAX_LEDS]; 
// Counter for the number of IDs in the buffer
static uint16_t led_ids_received = 0;
// Expected number of chunks for the current command
static uint8_t total_chunks_expected = 0;
// Number of chunks received so far
static uint8_t chunks_received = 0;
// ID of the current multi-packet command
static uint8_t current_cmd_id = 0xFF; 
// Current operational mode of the controller
static controller_mode_t current_mode = MODE_IDLE;

// --- Frame assembly state for CMD_STREAM_FRAME ---
#define MAX_CHUNKS 32 // Max chunks per frame
static uint8_t frame_buffer[MAX_LEDS * 3];
static uint32_t received_chunks_mask = 0;
static uint8_t expected_frame_chunks = 0;
static uint8_t current_frame_id = 0xFF;


static void udp_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = 0;
    struct sockaddr_in6 dest_addr;

    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(UDP_PORT);
    ip_protocol = IPPROTO_IP;

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket created");

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    }
    ESP_LOGI(TAG, "Socket bound, port %d", UDP_PORT);

    while (1) {
        ESP_LOGI(TAG, "Waiting for data");
        struct sockaddr_in6 source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

        // Error occurred during receiving
        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }

        // Check for GT signature and minimum length for a command
        if (len >= 3 && rx_buffer[0] == GT_HEADER_0 && rx_buffer[1] == GT_HEADER_1) {
            uint8_t cmd = rx_buffer[2];
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
            ESP_LOGI(TAG, "Received command %d from %s", cmd, addr_str);

            switch (cmd) {
                case CMD_DISCOVERY: {
                    esp_netif_ip_info_t ip_info;
                    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                    if (netif) {
                        esp_netif_get_ip_info(netif, &ip_info);
                        
                        // ip_info.ip.addr is in network byte order (big-endian)
                        uint8_t* ip_bytes = (uint8_t*)&ip_info.ip.addr;
                        uint8_t last_ip_byte = ip_bytes[3];

                        char response[4] = {GT_HEADER_0, GT_HEADER_1, CMD_DISCOVERY, last_ip_byte};
                        int err = sendto(sock, response, sizeof(response), 0, (struct sockaddr *)&source_addr, socklen);
                        if (err < 0) {
                            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                        }
                    } else {
                        ESP_LOGE(TAG, "STA netif not found");
                    }
                    break;
                }
                case CMD_GET_CONFIG: {
                    app_config_t config;
                    storage_load_config(&config);

                    uint8_t response[5] = {GT_HEADER_0, GT_HEADER_1, CMD_GET_CONFIG, config.brightness, config.power_state};
                    int err = sendto(sock, response, sizeof(response), 0, (struct sockaddr *)&source_addr, socklen);
                    if (err < 0) {
                        ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
                    }
                    break;
                }
                case CMD_SET_CONFIG: {
                    if (len >= 5) {
                        app_config_t config;
                        storage_load_config(&config);

                        config.brightness = rx_buffer[3];
                        config.power_state = rx_buffer[4];

                        storage_save_config(&config);
                        ESP_LOGI(TAG, "Saved new config: Brightness=%d, Power=%d", config.brightness, config.power_state);
                    } else {
                        ESP_LOGW(TAG, "CMD_SET_CONFIG packet too short: %d bytes", len);
                    }
                    break;
                }
                case CMD_SET_LEDS: {
                    // Header: cmdId (1), total_chunks (1), chunk_idx (1) -> 3 bytes
                    if (len < 6) { 
                        ESP_LOGW(TAG, "CMD_SET_LEDS packet too short: %d bytes", len);
                        break;
                    }

                    uint8_t cmd_id = rx_buffer[3];
                    uint8_t total_chunks = rx_buffer[4];
                    uint8_t chunk_idx = rx_buffer[5];

                    // If this is a new command sequence, reset the assembly state
                    if (cmd_id != current_cmd_id) {
                        current_cmd_id = cmd_id;
                        led_ids_received = 0;
                        chunks_received = 0;
                        total_chunks_expected = total_chunks;
                        ESP_LOGI(TAG, "New command sequence started. ID: %d, Total Chunks: %d", cmd_id, total_chunks);
                    }

                    // The rest of the payload contains LED IDs (uint16_t)
                    uint8_t* payload = (uint8_t*)rx_buffer + 6;
                    int payload_len = len - 6;
                    int num_ids_in_packet = payload_len / 2;

                    // Check if the buffer has enough space
                    if (led_ids_received + num_ids_in_packet > MAX_LEDS) {
                        ESP_LOGE(TAG, "LED ID buffer overflow detected. Aborting command.");
                        // Reset state to prevent partial updates
                        current_cmd_id = 0xFF; 
                        break;
                    }

                    // Copy LED IDs from payload to our assembly buffer
                    memcpy(&led_id_buffer[led_ids_received], payload, payload_len);
                    led_ids_received += num_ids_in_packet;
                    chunks_received++;

                    ESP_LOGI(TAG, "Chunk %d/%d received. Got %d IDs. Total IDs so far: %d", 
                             chunk_idx + 1, total_chunks, num_ids_in_packet, led_ids_received);

                    // If all chunks have been received, update the LEDs
                    if (chunks_received == total_chunks_expected) {
                        ESP_LOGI(TAG, "All chunks received. Updating %d LEDs.", led_ids_received);
                        current_mode = MODE_STATIC;
                        
                        led_driver_clear();
                        for (int i = 0; i < led_ids_received; i++) {
                            // Assuming color is white for now, as it's not in the protocol
                            led_driver_set_pixel(led_id_buffer[i], 255, 255, 255);
                        }
                        led_driver_update();
                        
                        // Reset state for the next command
                        current_cmd_id = 0xFF; 
                    }
                    break;
                }
                case CMD_STREAM_FRAME: {
                    // Packet format: [Headers(2)] [Cmd(1)] [CRC8(1)] [FrameID(1)] [TotalChunks(1)] [ChunkIdx(1)] [Payload...]
                    if (len < 7) {
                        ESP_LOGW(TAG, "CMD_STREAM_FRAME packet too short: %d bytes", len);
                        break;
                    }

                    uint8_t received_crc = rx_buffer[3];
                    uint8_t calculated_crc = crc8((uint8_t*)rx_buffer + 4, len - 4);

                    if (received_crc != calculated_crc) {
                        ESP_LOGW(TAG, "CRC mismatch. Got %02X, calculated %02X. Packet dropped.", received_crc, calculated_crc);
                        break;
                    }

                    uint8_t frame_id = rx_buffer[4];
                    uint8_t total_chunks = rx_buffer[5];
                    uint8_t chunk_idx = rx_buffer[6];
                    uint8_t* payload = (uint8_t*)rx_buffer + 7;
                    int payload_len = len - 7;

                    // If this is the first chunk of a new frame, reset state
                    if (frame_id != current_frame_id) {
                        current_frame_id = frame_id;
                        received_chunks_mask = 0;
                        expected_frame_chunks = total_chunks;
                        ESP_LOGI(TAG, "New frame sequence started. ID: %d, Total Chunks: %d", frame_id, total_chunks);
                    }

                    if (chunk_idx >= MAX_CHUNKS) {
                        ESP_LOGE(TAG, "Chunk index %d out of bounds (max %d)", chunk_idx, MAX_CHUNKS - 1);
                        break;
                    }
                    
                    // Check if we've already received this chunk
                    if ((received_chunks_mask & (1 << chunk_idx))) {
                        ESP_LOGW(TAG, "Duplicate chunk %d for frame %d received.", chunk_idx, frame_id);
                        break;
                    }

                    // Calculate offset and copy data
                    // Assuming each chunk for streaming is smaller than for CMD_SET_LEDS
                    // and that the client knows the chunk size.
                    // Let's assume a fixed chunk payload size for simplicity, e.g., 96 bytes.
                    // This needs to be coordinated with the client.
                    int chunk_size = 96; // Example chunk size
                    int offset = chunk_idx * chunk_size;

                    if (offset + payload_len > sizeof(frame_buffer)) {
                        ESP_LOGE(TAG, "Frame buffer overflow detected. Aborting frame.");
                        current_frame_id = 0xFF; // Invalidate frame
                        break;
                    }

                    memcpy(frame_buffer + offset, payload, payload_len);
                    
                    // Mark chunk as received
                    received_chunks_mask |= (1 << chunk_idx);

                    // Check if all chunks for the frame have been received
                    uint32_t all_chunks_mask = (1 << expected_frame_chunks) - 1;
                    if (received_chunks_mask == all_chunks_mask) {
                        ESP_LOGI(TAG, "Frame %d complete. Displaying.", frame_id);
                        current_mode = MODE_STREAMING;
                        led_driver_show_frame(frame_buffer, sizeof(frame_buffer));
                        
                        // Ready for the next frame
                        current_frame_id = 0xFF; 
                    }
                    break;
                }
                default:
                    ESP_LOGW(TAG, "Unknown command: %d", cmd);
                    break;
            }
        } else if (len > 0) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
            ESP_LOGW(TAG, "Received invalid/short packet from %s: len %d", addr_str, len);
        }
    }

    if (sock != -1) {
        ESP_LOGE(TAG, "Shutting down socket and restarting...");
        shutdown(sock, 0);
        close(sock);
    }
    vTaskDelete(NULL);
}

void udp_server_start(void)
{
    xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 5, NULL);
}
