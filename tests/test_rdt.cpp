#include <iostream>
#include <winsock2.h>
#include "RdtHeader.h"
#include "CheckSum.h"

int main()
{
    // Need to initialize Winsock on Windows for htonl/htons to work reliably
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }

    std::cout << "--- Starting RDT Serialization Test ---" << std::endl;

    // 1. Create a fake packet and fill the header
    RdtPacket original_packet;
    original_packet.header.seq_num = 42;
    original_packet.header.ack_num = 100;
    original_packet.header.flags = FLAG_DATA;
    original_packet.header.window_size = 8;
    original_packet.header.payload_len = 500;
    original_packet.header.reserved = 0;
    original_packet.header.checksum = 0; // Checksum is 0 before calculation

    // 2. Serialize it into a raw buffer
    char buffer[sizeof(RdtHeader)];
    serializeHeader(original_packet.header, buffer);

    // 3. Calculate checksum
    uint16_t computed_checksum = internetChecksum((const uint8_t*)buffer, sizeof(RdtHeader));
    original_packet.header.checksum = computed_checksum;
    
    // Serialize again, this time WITH the computed checksum
    serializeHeader(original_packet.header, buffer);

    // 4. Deserialize it back into a new struct
    RdtHeader round_trip = deserializeHeader(buffer);

    // 5. Assert values match!
    bool success = true;
    std::cout << "Seq Num: " << original_packet.header.seq_num << " -> " << round_trip.seq_num << std::endl;
    if (original_packet.header.seq_num != round_trip.seq_num) success = false;

    std::cout << "Ack Num: " << original_packet.header.ack_num << " -> " << round_trip.ack_num << std::endl;
    if (original_packet.header.ack_num != round_trip.ack_num) success = false;

    std::cout << "Flags: " << (int)original_packet.header.flags << " -> " << (int)round_trip.flags << std::endl;
    if (original_packet.header.flags != round_trip.flags) success = false;

    std::cout << "Window: " << original_packet.header.window_size << " -> " << round_trip.window_size << std::endl;
    if (original_packet.header.window_size != round_trip.window_size) success = false;

    std::cout << "Checksum: " << computed_checksum << " -> " << round_trip.checksum << std::endl;
    if (original_packet.header.checksum != round_trip.checksum) success = false;

    if (success) {
        std::cout << "\nSUCCESS! Serialization, Deserialization, and Checksum worked perfectly!" << std::endl;
    } else {
        std::cout << "\nFAILED! Some values did not match." << std::endl;
    }

    // 6. Demonstrate corruption detection
    std::cout << "\n--- Testing Checksum Corruption ---" << std::endl;
    
    // Flip a random bit in the serialized buffer (simulating network error)
    buffer[2] = buffer[2] ^ 0b00000001; 
    
    uint16_t broken_checksum = internetChecksum((const uint8_t*)buffer, sizeof(RdtHeader));
    if (broken_checksum != 0) { // If the checksum over the whole packet is NOT 0, it means it's corrupted
        std::cout << "Corruption detected successfully! (Checksum failed)" << std::endl;
    } else {
        std::cout << "Corruption NOT detected! Checksum logic failed." << std::endl;
    }

    WSACleanup();
    return 0;
}
