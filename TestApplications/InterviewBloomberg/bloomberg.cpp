#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <chrono>
#include <cstdint>
#include <ctime>

#ifdef __GNUC__
#define sscanf_s sscanf
#endif

struct pcap_hdr_s {
    uint32_t magic_number;   /* magic number */
    uint16_t version_major;  /* major version number */
    uint16_t version_minor;  /* minor version number */
    int32_t  thiszone;       /* GMT to local correction */
    uint32_t sigfigs;        /* accuracy of timestamps */
    uint32_t snaplen;        /* max length of captured packets, in octets */
    uint32_t network;        /* data link type */
};

struct pcaprec_hdr_s {
    uint32_t ts_sec;         /* timestamp seconds */
    uint32_t ts_usec;        /* timestamp microseconds or nanoseconds */
    uint32_t incl_len;       /* number of octets of packet saved in file */
    uint32_t orig_len;       /* actual length of packet */
};

struct eth_hdr_s {
    uint8_t  dst_mac[6];   // Destination MAC address
    uint8_t  src_mac[6];   // Source MAC address
    uint16_t ethertype;    // 0x0800 for IPv4, 0x86DD for IPv6
};

struct ipv4_hdr_s {
    uint8_t  version_ihl;      // Version (4 bits) + Header Length (4 bits)
    uint8_t  tos;              // Type of Service
    uint16_t total_length;     // Entire packet length (header + payload)
    uint16_t identification;   // Fragment ID
    uint16_t flags_offset;     // Flags (3 bits) + Fragment Offset (13 bits)
    uint8_t  ttl;              // Time to Live
    uint8_t  protocol;         // 17 for UDP, 6 for TCP
    uint16_t checksum;         // Header checksum
    uint32_t src_ip;           // Source IP address
    uint32_t dst_ip;           // Destination IP address
};

struct udp_hdr_s {
    uint16_t src_port;    // Source port
    uint16_t dst_port;    // Destination port
    uint16_t length;      // Datagram length (header + payload)
    uint16_t checksum;    // UDP checksum (0 if unused)
}; 

// payload = f"{ts}|{exchange_id}|{instrument}|{mtype}|{value}".encode("ascii")
struct Payload {
    void load(const std::string& line) {
        size_t pos = 0;
        size_t prev_pos = 0;
        int field_index = 0;
        while ((pos = line.find('|', prev_pos)) != std::string::npos) {
            std::string field = line.substr(prev_pos, pos - prev_pos);
            switch (field_index) {
            case 0:
                {
                    int year, month, day;
                    int hour, minute, second;
                    long nanoseconds_part = 0;
                    if (sscanf_s(
                        field.c_str(),
                        "%4d-%2d-%2dT%2d:%2d:%2d.%9ldZ",
                        &year, &month, &day,
                        &hour, &minute, &second,
                        &nanoseconds_part) != 7)
                    {
                        throw std::runtime_error("Invalid timestamp format");
                    }

                    std::tm tm{};
                    tm.tm_year = year - 1900;
                    tm.tm_mon = month - 1;
                    tm.tm_mday = day;
                    tm.tm_hour = hour;
                    tm.tm_min = minute;
                    tm.tm_sec = second;

                    time_t time = std::mktime(&tm);

                    timestamp = std::chrono::system_clock::from_time_t(time) +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(nanoseconds_part));
                }
                
                break;
            case 1:
                exchange_id = field;
                break;
            case 2:
                instrument = field;
                break;
            case 3:
                mtype = field;
                break;
            case 4:
                value = field;
                break;
            }
            prev_pos = pos + 1;
            field_index++;
        }
        // Handle the last field
        if (field_index == 4 && prev_pos < line.size()) {
            value = line.substr(prev_pos);
        }
    }

    std::chrono::system_clock::time_point timestamp;
    std::string exchange_id;
    std::string instrument;
    std::string mtype;
    std::string value;
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <input_file>" << std::endl;
        return 1;
    }
    std::ifstream inputFile(argv[1], std::ios::binary);
    if (!inputFile) {
        std::cerr << "Error opening file: " << argv[1] << std::endl;
        return 1;
    }

    pcap_hdr_s header;

    if (inputFile.eof()) {
        return 2;
    }

    inputFile.read((char*)&header, sizeof(header));

    bool same_endiness(true);
    
    switch (header.magic_number) {
    case 0xa1b2c3d4:
        std::cout << "Same endiness" << std::endl << std::endl;
        break;
    case 0xd4c3b2a1:
        // Swapped
        same_endiness = false;
        std::cout << "Reverced endiness" << std::endl << std::endl;
        break;
    default:
        std::cout << "The magic is gone!" << std::hex << header.magic_number << std::endl;
        return 3;
    }

    pcaprec_hdr_s pheader;
    eth_hdr_s eth_header;
    ipv4_hdr_s ipv4_header;
    udp_hdr_s udp_header;

    std::string line;
    char buffer[10000];
    Payload payload;
    try {
        while (!inputFile.eof()) {
            inputFile.read((char*)&pheader, sizeof(pheader));
            inputFile.read((char*)&eth_header, sizeof(eth_header));
            inputFile.read((char*)&ipv4_header, sizeof(ipv4_header));
            inputFile.read((char*)&udp_header, sizeof(udp_header));

            size_t length = pheader.incl_len - (sizeof(eth_header) + sizeof(ipv4_header) + sizeof(udp_header));
            inputFile.read(buffer, length);

            line.assign(buffer, length);

            std::cout << line << std::endl; // For demonstration, just print the line

            payload.load(line);

            std::cout << "    Parsed Payload: Timestamp = " << payload.timestamp.time_since_epoch().count() <<
            ", Exchange ID = " << payload.exchange_id <<
            ", Instrument = " << payload.instrument <<
            ", Message Type = " << payload.mtype <<
            ", Value = " << payload.value << std::endl;

        }
    }
    catch (const std::exception& ex) {
        std::cerr << "Error processing file: " << ex.what() << std::endl;
        return 4;
    }

    inputFile.close();
    return 0;
}
