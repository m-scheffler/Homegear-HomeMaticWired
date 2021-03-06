/* Copyright 2013-2019 Homegear GmbH
 *
 * Homegear is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Homegear is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Homegear.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */

#ifndef HMWIREDPACKET_H_
#define HMWIREDPACKET_H_

#include <homegear-base/BaseLib.h>

#include <map>

namespace HMWired
{
enum class HMWiredPacketType {none = 0, iMessage, ackMessage, system, discovery, discoveryResponse};

class CRC16
{
public:
	virtual ~CRC16() {}
	static void init();
	static uint16_t calculate(std::vector<uint8_t>& data);
private:
	static std::map<uint16_t, uint16_t> _crcTable;

	CRC16() {};
	static void initCRCTable();
};

class HMWiredPacket : public BaseLib::Systems::Packet
{
public:
    //Properties
    HMWiredPacket();
    HMWiredPacket(std::string packet, int64_t timeReceived = 0);
    HMWiredPacket(std::vector<uint8_t>& packet, int64_t timeReceived = 0, bool removeEscapes = false);
    HMWiredPacket(std::vector<uint8_t>& packet, bool gatewayPacket, int64_t timeReceived = 0, int32_t senderAddress = 0, int32_t destinationAddress = 0);
    HMWiredPacket(HMWiredPacketType type, int32_t senderAddress, int32_t destinationAddress, bool synchronizationBit, uint8_t senderMessageCounter, uint8_t receiverMessageCounter, uint8_t addressMask, std::vector<uint8_t>& payload);
    virtual ~HMWiredPacket();

    uint8_t length() { return _length; }
    int32_t senderAddress() { return _senderAddress; }
    int32_t destinationAddress() { return _destinationAddress; }
    uint8_t controlByte() { return _controlByte; }
    HMWiredPacketType type() { return _type; }
    uint8_t messageType() { if(_payload.empty()) return 0; else return _payload.at(0); }
    uint16_t checksum() { return _checksum; }
    uint8_t addressMask() { return _addressMask; }
    uint8_t senderMessageCounter() { return _senderMessageCounter; }
    uint8_t receiverMessageCounter() { return _receiverMessageCounter; }
    bool synchronizationBit() { return _synchronizationBit; }
    std::string hexString();
    std::vector<uint8_t> byteArray();
    std::vector<uint8_t>& payload() { return _payload; }
    std::vector<char> byteArraySigned();
    std::vector<char> byteArrayLgw();

    void import(std::vector<uint8_t>& packet, bool removeEscapes = false);
    void import(std::string packetHex);
    std::vector<uint8_t> getPosition(double index, double size, int32_t mask);
    void setPosition(double index, double size, std::vector<uint8_t>& value);
private:
    static const std::array<uint8_t, 9> _bitmask;

    //Packet content
    uint8_t _length = 0;
    int32_t _senderAddress = 0;
    int32_t _destinationAddress = 0;
    uint8_t _controlByte = 0;
    std::vector<uint8_t> _packet;
    std::vector<uint8_t> _escapedPacket;
    std::vector<uint8_t> _payload;
    HMWiredPacketType _type = HMWiredPacketType::none;
    uint16_t _checksum = 0;
    uint8_t _addressMask = 0;
    uint8_t _senderMessageCounter = 0;
    uint8_t _receiverMessageCounter = 0;
    bool _synchronizationBit = false;
    //End packet content

    void init();
    void reset();
    void escapePacket();
    void escapePacket(std::vector<uint8_t>& result, const std::vector<uint8_t>& packet);
    void escapePacket(std::vector<char>& result, const std::vector<char>& packet);
    std::vector<uint8_t> unescapePacket(std::vector<uint8_t>& packet);
    void generateControlByte();
};

} /* namespace HMWired */
#endif /* HMWIREDPACKET_H_ */
