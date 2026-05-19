/* product-mini/platforms/zephyr/dictionary-log/wasm-apps/log_app_network.c */
#include "wasm_log.h"

static void net_init(void)
{
    int32_t major = 2, minor = 5, patch = 1;
    uint32_t heap_size = 65536, stack_size = 8192;

    /* Intentional duplicates with sensor app */
    LOG_INF("Firmware version %d.%d.%d built for qemu_x86", major, minor, patch);
    LOG_INF("Memory config: heap=%u bytes, stack=%u bytes", heap_size, stack_size);

    /* Network-specific init */
    LOG_INF("=== Network Stack starting ===");
    LOG_INF("TCP listen port=%d, max connections=%d", 8080, 4);
    LOG_DBG("UDP socket pool: %d sockets allocated, buffer=%u bytes each", 8, 1500);
    LOG_INF("DNS resolver: primary server configured, timeout=%d ms", 5000);
    LOG_DBG("ARP cache: %d entries max, timeout=%d seconds", 32, 300);
    LOG_INF("Network interface: link speed=%u kbps, duplex=%d", 100000, 1);
    LOG_DBG("IP stack: MTU=%u, fragment reassembly buffer=%u bytes", 1500, 4096);
    LOG_INF("TLS context initialized: cipher suites=%d, session cache=%d", 4, 16);
}

static void net_traffic(void)
{
    char tx_buf[64];
    char *rx_buf = tx_buf + 32;
    void *socket_ctx = (void *)0x20004000;

    LOG_DBG("TX buffer at %p, RX buffer at %p", tx_buf, rx_buf);
    LOG_DBG("Socket context: %p", socket_ctx);
    LOG_INF("TCP connection established: remote port=%d, local port=%d", 443, 49152);
    LOG_DBG("TLS handshake complete: protocol version=%d, cipher=0x%x", 0x0304, 0x1301);
    LOG_INF("HTTP request sent: method=%d, content length=%d bytes", 1, 256);
    LOG_DBG("TCP window: send=%u receive=%u, congestion window=%u", 65535, 32768, 16384);
    LOG_DBG("Packet TX: seq=%u ack=%u length=%d flags=0x%x", 1000, 500, 256, 0x18);
    LOG_INF("Data received: %d bytes on socket %d, buffer fill=%d percent", 1024, 3, 45);
    LOG_DBG("Retransmission triggered: seq=%u, timeout=%d ms, attempt=%d of %d", 1000, 200, 1, 3);
    LOG_WRN("Socket %d send buffer full: queued=%u limit=%u bytes", 3, 65536, 65536);
    LOG_ERR("Connection reset by peer: socket=%d, error code=%d", 3, -104);
    LOG_DBG("TCP state transition: socket=%d, old state=%d new state=%d", 3, 4, 8);
}

static void net_errors(void)
{
    const char *hostname = "telemetry.example.com";

    LOG_ERR("DNS resolution failed for '%s': timeout after %d ms", hostname, 5000);
    LOG_WRN("ICMP destination unreachable: type=%d code=%d from hop %d", 3, 1, 5);
    LOG_ERR("TLS certificate validation failed: error=0x%x, chain depth=%d", 0x2700, 1);
    LOG_WRN("Network congestion detected: RTT=%d ms, packet loss=%d percent", 450, 12);
    LOG_ERR("ARP resolution timeout: retries=%d, interface=%d", 3, 0);
    LOG_DBG("TCP keepalive timeout: socket=%d, idle=%d seconds, max=%d", 5, 120, 60);

    /* Intentional duplicates with sensor app */
    LOG_INF("Initialization complete: %d subsystems ready, %d warnings", 3, 1);
    LOG_ERR("Stack overflow detected: task %d, usage=%u of %u bytes", 1, 3800, 4096);
}

int main(void)
{
    net_init();
    net_traffic();
    net_errors();
    return 0;
}
