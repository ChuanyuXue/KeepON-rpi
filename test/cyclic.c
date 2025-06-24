// time_triggered_v3.c
// Use tc-etf API to set the time of the packet. v2 uses socket option SO_TXTIME.

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PTP_CLOCK_NAME "/dev/ptp0"
#define INTERFACE_NAME "eth0"
#define SO_TXTIME 61
#define DEFAULT_PACKET_SIZE 1514
#define DEFAULT_PERIOD_NS 1000000000ULL
#define DEFAULT_DELTA_NS 120000ULL
#define TEST_IP "192.168.0.66"
#define TEST_PORT 12345
#define SOURCE_PORT 54321
#define DEFAULT_PRIORITY 8

int main(int argc, char *argv[]) {
    int sock_fd;
    struct timespec ts;
    struct sockaddr_ll addr;
    struct ifreq ifr;
    size_t packet_size = DEFAULT_PACKET_SIZE;
    uint64_t period_ns = DEFAULT_PERIOD_NS;
    uint64_t delta_ns = DEFAULT_DELTA_NS;
    uint64_t arg_nanosecond_component = 0;
    int custom_first_packet_timing_enabled = 0;
    uint64_t first_packet_target_time_ns = 0;
    char *msg = NULL;
    uint64_t scheduled_time;
    struct cmsghdr *cmsg;
    char control[CMSG_SPACE(sizeof(uint64_t))];
    struct msghdr mhdr;
    struct iovec iov;
    uint64_t last_actual_sent_time_ns = 0;  // Stores the scheduled time of the last sent packet
    int is_first_packet = 1;                // Flag to handle first packet logic
    uint32_t sequence_number = 0;           // Initialize sequence number
    int priority = DEFAULT_PRIORITY;        // Add priority variable

    // VLAN parameters
    uint16_t pcp_val = 0;
    uint16_t vid_val = 0;
    int vlan_enabled = 0;
    int attempted_vlan_args = 0;

    // Parse command line arguments
    // Order: ./program [period_ns] [delta_ns] [packet_size] [priority] [nanosecond_component] [pcp] [vid]
    if (argc > 1) period_ns = strtoull(argv[1], NULL, 10);
    if (argc > 2) delta_ns = strtoull(argv[2], NULL, 10);
    if (argc > 3) packet_size = strtoull(argv[3], NULL, 10);
    if (argc > 4) priority = atoi(argv[4]);

    if (argc > 5) {
        char *endptr;
        // Using unsigned long long for strtoull compatibility, then casting
        unsigned long long parsed_ns_component = strtoull(argv[5], &endptr, 10);

        if (argv[5][0] != '\0' && *endptr == '\0' && parsed_ns_component < 1000000000ULL) {
            arg_nanosecond_component = (uint64_t)parsed_ns_component;
            custom_first_packet_timing_enabled = 1;
        } else if (argv[5][0] != '\0') {
            fprintf(stderr, "Warning: Invalid nanosecond component '%s' provided in argv[5]. Must be a number < 1,000,000,000. Using default timing for first packet.\n", argv[5]);
        }
    }

    if (argc > 6) attempted_vlan_args = 1;

    if (argc > 7) {
        char *endptr_pcp, *endptr_vid;
        long pcp_long = strtol(argv[6], &endptr_pcp, 10);
        long vid_long = strtol(argv[7], &endptr_vid, 10);

        if (*endptr_pcp == '\0' && *endptr_vid == '\0' &&
            pcp_long >= 0 && pcp_long <= 7 &&
            vid_long >= 0 && vid_long <= 4095) {
            pcp_val = (uint16_t)pcp_long;
            vid_val = (uint16_t)vid_long;
            vlan_enabled = 1;
        } else {
            fprintf(stderr, "Warning: Invalid PCP or VID values provided. PCP must be 0-7, VID 0-4095. VLAN tagging disabled.\n");
            fprintf(stderr, "  Provided PCP: '%s', VID: '%s'\n", argv[6], argv[7]);
        }
    }

    printf("Configuration:\n");
    printf("  Period: %lu ns\n", period_ns);
    printf("  Delta: %lu ns\n", delta_ns);
    printf("  Requested Packet size: %zu bytes\n", packet_size);
    printf("  Priority: %d\n", priority);
    if (custom_first_packet_timing_enabled) {
        printf("  First packet timing: Current TAI + 3 seconds + %lu ns (nanosecond component from argv[5]).\n", arg_nanosecond_component);
    } else {
        printf("  First packet timing: Default (scheduled on the next period boundary from current time).\n");
        if (argc > 5 && argv[5][0] != '\0' && !custom_first_packet_timing_enabled) {
            printf("  (Note: An invalid nanosecond component was provided via argv[5] and was ignored.)\n");
        }
    }

    if (vlan_enabled) {
        printf("  VLAN Tagging: Enabled (PCP=%u, VID=%u)\n", pcp_val, vid_val);
    } else {
        printf("  VLAN Tagging: Disabled.\n");
        if (attempted_vlan_args) {
            if (argc == 7) {
                printf("  (Note: For VLAN, both PCP (arg 6) and VID (arg 7) must be specified after offset. VID argument missing.)\n");
            } else if (argc > 7) {
                printf("  (Note: For VLAN, ensure valid PCP [0-7] (arg 6) and VID [0-4095] (arg 7) were provided.)\n");
            }
        }
    }

    // Validate packet size
    // Headers: DestMAC(6) + SrcMAC(6) + [VLAN TPID(2)+TCI(2)] + PayloadEtherType(2) + SeqNum(4)
    size_t headers_base_len = 12 + 2 + sizeof(uint32_t);
    size_t vlan_tag_len = vlan_enabled ? 4 : 0;
    size_t total_min_len = headers_base_len + vlan_tag_len;

    if (packet_size < total_min_len) {
        fprintf(stderr, "Error: Packet size %zu is too small for headers and sequence number.\n", packet_size);
        fprintf(stderr, "Minimum required for current config (MACs + %sVLAN Tag + EtherType + SeqNo): %zu bytes.\n", (vlan_enabled ? "" : "No "), total_min_len);
        exit(1);
    }
    size_t max_standard_frame_size = 1514 + (vlan_enabled ? 4 : 0);
    if (packet_size > max_standard_frame_size) {
        fprintf(stderr, "Warning: Packet size %zu exceeds standard Ethernet frame size of %zu bytes for this configuration (excluding FCS).\n", packet_size, max_standard_frame_size);
    }

    // Allocate message buffer dynamically
    msg = (char *)malloc(packet_size);
    if (msg == NULL) {
        perror("Failed to allocate message buffer");
        exit(1);
    }

    // Create RAW socket instead of UDP
    sock_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock_fd < 0) {
        perror("Failed to create socket");
        exit(1);
    }

    // Set socket reuse
    int reuse = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Failed to set SO_REUSEADDR");
        exit(1);
    }

    // Increase socket buffer size
    int sendbuff = 1024 * 1024;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF, &sendbuff, sizeof(sendbuff)) < 0) {
        perror("Failed to set socket send buffer size");
        exit(1);
    }

    // Set priority for ETF
    if (setsockopt(sock_fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) < 0) {
        perror("Failed to set socket priority");
        exit(1);
    }

    // Enable SO_TXTIME option with TAI clock
    static struct sock_txtime sk_txtime;
    sk_txtime.clockid = CLOCK_TAI;
    sk_txtime.flags = 0;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_TXTIME, &sk_txtime, sizeof(sk_txtime)) < 0) {
        perror("Failed to set SO_TXTIME option");
        exit(1);
    }

    // Enable hardware timestamping
    int timestamping_flags = SOF_TIMESTAMPING_TX_HARDWARE | 
                            SOF_TIMESTAMPING_RAW_HARDWARE |
                            SOF_TIMESTAMPING_TX_SOFTWARE;

    if (setsockopt(sock_fd, SOL_SOCKET, SO_TIMESTAMPING, 
                   &timestamping_flags, sizeof(timestamping_flags)) < 0) {
        perror("Failed to set SO_TIMESTAMPING option");
        exit(1);
    }

    // Enable timestamping on the network interface
    struct ifreq hwtstamp_ifr;
    struct hwtstamp_config hwtstamp_cfg;

    memset(&hwtstamp_ifr, 0, sizeof(hwtstamp_ifr));
    memset(&hwtstamp_cfg, 0, sizeof(hwtstamp_cfg));

    strncpy(hwtstamp_ifr.ifr_name, INTERFACE_NAME, IFNAMSIZ - 1);
    hwtstamp_cfg.tx_type = HWTSTAMP_TX_ON;  // Enable TX timestamping
    hwtstamp_cfg.rx_filter = HWTSTAMP_FILTER_NONE;  // We don't need RX timestamping for this test

    hwtstamp_ifr.ifr_data = (char *)&hwtstamp_cfg;

    if (ioctl(sock_fd, SIOCSHWTSTAMP, &hwtstamp_ifr) < 0) {
        perror("Warning: Failed to enable hardware timestamping on interface");
        printf("Hardware timestamping may not be supported or already enabled\n");
        // Don't exit - continue without hardware timestamping
    }

    printf("Hardware timestamping enabled on interface %s\n", INTERFACE_NAME);

    // Replace UDP binding and address setup with interface binding for raw socket
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, INTERFACE_NAME, IFNAMSIZ - 1);
    if (ioctl(sock_fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("Failed to get interface index");
        exit(1);
    }

    // Set up link layer address
    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_ifindex = ifr.ifr_ifindex;
    addr.sll_halen = ETH_ALEN;
    addr.sll_protocol = htons(ETH_P_ALL);

    while (1) {
        // Get current time
        if (clock_gettime(CLOCK_TAI, &ts) < 0) {
            perror("Failed to get time");
            break;
        }

        uint64_t now_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        uint64_t current_epoch_scheduled_time;

        if (is_first_packet) {
            if (custom_first_packet_timing_enabled) {
                uint64_t target_base_seconds_ns = (ts.tv_sec + 3) * 1000000000ULL;
                first_packet_target_time_ns = target_base_seconds_ns + arg_nanosecond_component;

                current_epoch_scheduled_time = first_packet_target_time_ns;

                if (current_epoch_scheduled_time <= now_ns) {
                    fprintf(stderr, "Warning: Calculated initial target time %lu ns (based on TAI + 3s + %lu ns component) is in the past or now (current TAI: %lu ns). Will attempt to send ASAP with this timestamp.\n",
                            current_epoch_scheduled_time, arg_nanosecond_component, now_ns);
                }
            } else {
                current_epoch_scheduled_time = ((now_ns / period_ns) + 1) * period_ns;
                first_packet_target_time_ns = current_epoch_scheduled_time;
            }
        } else {
            current_epoch_scheduled_time = last_actual_sent_time_ns + period_ns;
        }

        if (current_epoch_scheduled_time > last_actual_sent_time_ns || is_first_packet) {
            scheduled_time = current_epoch_scheduled_time;

            uint64_t sleep_ns = 0;
            if (scheduled_time > now_ns) {
                uint64_t time_until_tx_ns = scheduled_time - now_ns;
                uint64_t wake_before_ns = delta_ns;
                if (time_until_tx_ns > wake_before_ns) {
                    sleep_ns = time_until_tx_ns - wake_before_ns;
                }
            }

            if (sleep_ns > 0) {
                struct timespec sleep_time = {
                    .tv_sec = sleep_ns / 1000000000ULL,
                    .tv_nsec = sleep_ns % 1000000000ULL};
                nanosleep(&sleep_time, NULL);
            }

            // Prepare message for raw Ethernet
            memset(msg, 0, packet_size);

            size_t current_offset = 0;

            // Destination MAC
            memcpy(msg + current_offset, "\x01\x02\x03\x04\x05\x06", 6);  // Placeholder Dest MAC
            current_offset += 6;
            // Source MAC
            memcpy(msg + current_offset, "\x07\x08\x09\x0A\x0B\x0C", 6);  // Placeholder Src MAC
            current_offset += 6;

            if (vlan_enabled) {
                // 802.1Q TPID (Tag Protocol Identifier) - 0x8100
                uint16_t tpid = htons(0x8100);
                memcpy(msg + current_offset, &tpid, sizeof(tpid));
                current_offset += sizeof(tpid);

                // TCI (Tag Control Information): PCP (3 bits), DEI (1 bit), VID (12 bits)
                uint16_t tci = (pcp_val << 13) | (0 << 12) /* DEI = 0 */ | (vid_val & 0x0FFF);
                tci = htons(tci);
                memcpy(msg + current_offset, &tci, sizeof(tci));
                current_offset += sizeof(tci);
            }

            // Original/Payload Ethertype (e.g., IPv4, ARP, or custom like 0x4399)
            uint16_t payload_ethertype = htons(0x4399);
            memcpy(msg + current_offset, &payload_ethertype, sizeof(payload_ethertype));
            current_offset += sizeof(payload_ethertype);

            // Add sequence number to the front of the payload area
            memcpy(msg + current_offset, &sequence_number, sizeof(sequence_number));

            sequence_number++;

            // Setup message header with link layer destination
            memset(&mhdr, 0, sizeof(mhdr));
            mhdr.msg_name = &addr;
            mhdr.msg_namelen = sizeof(addr);

            // Setup iovec
            iov.iov_base = msg;
            iov.iov_len = packet_size;
            mhdr.msg_iov = &iov;
            mhdr.msg_iovlen = 1;

            // Setup control message
            mhdr.msg_control = control;
            mhdr.msg_controllen = CMSG_SPACE(sizeof(uint64_t));

            cmsg = CMSG_FIRSTHDR(&mhdr);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_TXTIME;
            cmsg->cmsg_len = CMSG_LEN(sizeof(uint64_t));
            *(uint64_t *)CMSG_DATA(cmsg) = scheduled_time;

            // Send message with scheduled time
            if (sendmsg(sock_fd, &mhdr, 0) < 0) {
                perror("Failed to send message");
            } else {
                printf("Packet %d scheduled for: %lu ns\n", sequence_number, scheduled_time);
                if (is_first_packet) {
                    is_first_packet = 0;
                }

                // Retrieve timestamp
                struct msghdr err_msg;
                struct iovec err_iov;
                char err_control[512];
                struct cmsghdr *err_cmsg;
                char err_data[256];

                memset(&err_msg, 0, sizeof(err_msg));
                err_iov.iov_base = err_data;
                err_iov.iov_len = sizeof(err_data);
                err_msg.msg_iov = &err_iov;
                err_msg.msg_iovlen = 1;
                err_msg.msg_control = err_control;
                err_msg.msg_controllen = sizeof(err_control);

                // Try to receive error queue for timestamps
                if (recvmsg(sock_fd, &err_msg, MSG_ERRQUEUE | MSG_DONTWAIT) >= 0) {
                    for (err_cmsg = CMSG_FIRSTHDR(&err_msg); err_cmsg; err_cmsg = CMSG_NXTHDR(&err_msg, err_cmsg)) {
                        if (err_cmsg->cmsg_level == SOL_SOCKET && err_cmsg->cmsg_type == SCM_TIMESTAMPING) {
                            struct timespec *timestamps = (struct timespec *)CMSG_DATA(err_cmsg);
                            
                            // timestamps[0] = software timestamp
                            // timestamps[1] = legacy hardware timestamp
                            // timestamps[2] = raw hardware timestamp
                            
                            if (timestamps[2].tv_sec != 0 || timestamps[2].tv_nsec != 0) {
                                uint64_t hw_timestamp_ns = timestamps[2].tv_sec * 1000000000ULL + timestamps[2].tv_nsec;
                                printf("  HW timestamp: %lu ns\n", hw_timestamp_ns);
                            }
                        }
                    }
                }
            }
            last_actual_sent_time_ns = scheduled_time;
        } else {
            usleep(1);
        }
    }

    free(msg);
    close(sock_fd);

    return 0;
}