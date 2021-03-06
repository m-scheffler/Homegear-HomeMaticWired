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

#include "HMWiredCentral.h"
#include "GD.h"

#include <iomanip>

namespace HMWired {

HMWiredCentral::HMWiredCentral(ICentralEventSink* eventHandler) : BaseLib::Systems::ICentral(HMWIRED_FAMILY_ID, GD::bl, eventHandler)
{
	init();
}

HMWiredCentral::HMWiredCentral(uint32_t deviceID, std::string serialNumber, int32_t address, ICentralEventSink* eventHandler) : BaseLib::Systems::ICentral(HMWIRED_FAMILY_ID, GD::bl, deviceID, serialNumber, address, eventHandler)
{
	init();
}

HMWiredCentral::~HMWiredCentral()
{
	dispose();

	_updateFirmwareThreadMutex.lock();
	_bl->threadManager.join(_updateFirmwareThread);
	_updateFirmwareThreadMutex.unlock();
	_announceThreadMutex.lock();
	_bl->threadManager.join(_announceThread);
	_announceThreadMutex.unlock();
}

void HMWiredCentral::dispose(bool wait)
{
	try
	{
		if(_disposing) return;
		_disposing = true;
		GD::out.printDebug("Removing device " + std::to_string(_deviceId) + " from physical device's event queue...");
		if(GD::physicalInterface) GD::physicalInterface->removeEventHandler(_physicalInterfaceEventhandlers[GD::physicalInterface->getID()]);
		_stopWorkerThread = true;
		GD::out.printDebug("Debug: Waiting for worker thread of device " + std::to_string(_deviceId) + "...");
		_bl->threadManager.join(_workerThread);
	}
    catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void HMWiredCentral::init()
{
	try
	{
		if(_initialized) return; //Prevent running init two times
		_initialized = true;

		if(GD::physicalInterface) _physicalInterfaceEventhandlers[GD::physicalInterface->getID()] = GD::physicalInterface->addEventHandler((BaseLib::Systems::IPhysicalInterface::IPhysicalInterfaceEventSink*)this);

		_messageCounter[0] = 0; //Broadcast message counter
		_stopWorkerThread = false;
		_pairing = false;
		_updateMode = false;

		_bl->threadManager.start(_workerThread, true, _bl->settings.workerThreadPriority(), _bl->settings.workerThreadPolicy(), &HMWiredCentral::worker, this);
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
}

void HMWiredCentral::worker()
{
	try
	{
		std::chrono::milliseconds sleepingTime(10);
		uint32_t counter = 0;
		int32_t lastPeer;
		lastPeer = 0;
		//One loop on the Raspberry Pi takes about 30µs
		while(!_stopWorkerThread)
		{
			try
			{
				std::this_thread::sleep_for(sleepingTime);
				if(_stopWorkerThread) return;
				if(counter > 10000)
				{
					counter = 0;
					_peersMutex.lock();
					if(_peers.size() > 0)
					{
						int32_t windowTimePerPeer = _bl->settings.workerThreadWindow() / _peers.size();
						if(windowTimePerPeer > 2) windowTimePerPeer -= 2;
						sleepingTime = std::chrono::milliseconds(windowTimePerPeer);
					}
					_peersMutex.unlock();
				}
				_peersMutex.lock();
				if(!_peers.empty())
				{
					if(!_peers.empty())
					{
						std::unordered_map<int32_t, std::shared_ptr<BaseLib::Systems::Peer>>::iterator nextPeer = _peers.find(lastPeer);
						if(nextPeer != _peers.end())
						{
							nextPeer++;
							if(nextPeer == _peers.end()) nextPeer = _peers.begin();
						}
						else nextPeer = _peers.begin();
						lastPeer = nextPeer->first;
					}
				}
				_peersMutex.unlock();
				std::shared_ptr<HMWiredPeer> peer(getPeer(lastPeer));
				if(peer && !peer->deleting) peer->worker();
				counter++;
			}
			catch(const std::exception& ex)
			{
				_peersMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
			}
			catch(...)
			{
				_peersMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
			}
		}
	}
    catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void HMWiredCentral::loadPeers()
{
	try
	{
		std::shared_ptr<BaseLib::Database::DataTable> rows = _bl->db->getPeers(_deviceId);
		for(BaseLib::Database::DataTable::iterator row = rows->begin(); row != rows->end(); ++row)
		{
			int32_t peerID = row->second.at(0)->intValue;
			GD::out.printMessage("Loading HomeMatic Wired peer " + std::to_string(peerID));
			int32_t address = row->second.at(2)->intValue;
			std::shared_ptr<HMWiredPeer> peer(new HMWiredPeer(peerID, address, row->second.at(3)->textValue, _deviceId, this));
			if(!peer->load(this)) continue;
			if(!peer->getRpcDevice()) continue;
			_peersMutex.lock();
			_peers[peer->getAddress()] = peer;
			if(!peer->getSerialNumber().empty()) _peersBySerial[peer->getSerialNumber()] = peer;
			_peersById[peerID] = peer;
			_peersMutex.unlock();
		}
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    	_peersMutex.unlock();
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    	_peersMutex.unlock();
    }
}

void HMWiredCentral::loadVariables()
{
	try
	{
		std::shared_ptr<BaseLib::Database::DataTable> rows = _bl->db->getDeviceVariables(_deviceId);
		for(BaseLib::Database::DataTable::iterator row = rows->begin(); row != rows->end(); ++row)
		{
			_variableDatabaseIds[row->second.at(2)->intValue] = row->second.at(0)->intValue;
			switch(row->second.at(2)->intValue)
			{
			case 0:
				_firmwareVersion = row->second.at(3)->intValue;
				break;
			case 1:
				_centralAddress = row->second.at(3)->intValue;
				break;
			case 2:
				unserializeMessageCounters(row->second.at(5)->binaryValue);
				break;
			}
		}
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void HMWiredCentral::saveVariables()
{
	try
	{
		if(_deviceId == 0) return;
		saveVariable(0, _firmwareVersion);
		saveVariable(1, _centralAddress);
		saveMessageCounters(); //2
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

std::shared_ptr<HMWiredPeer> HMWiredCentral::getPeer(int32_t address)
{
	try
	{
		_peersMutex.lock();
		if(_peers.find(address) != _peers.end())
		{
			std::shared_ptr<HMWiredPeer> peer(std::dynamic_pointer_cast<HMWiredPeer>(_peers.at(address)));
			_peersMutex.unlock();
			return peer;
		}
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    _peersMutex.unlock();
    return std::shared_ptr<HMWiredPeer>();
}

std::shared_ptr<HMWiredPeer> HMWiredCentral::getPeer(uint64_t id)
{
	try
	{
		_peersMutex.lock();
		if(_peersById.find(id) != _peersById.end())
		{
			std::shared_ptr<HMWiredPeer> peer(std::dynamic_pointer_cast<HMWiredPeer>(_peersById.at(id)));
			_peersMutex.unlock();
			return peer;
		}
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    _peersMutex.unlock();
    return std::shared_ptr<HMWiredPeer>();
}

std::shared_ptr<HMWiredPeer> HMWiredCentral::getPeer(std::string serialNumber)
{
	try
	{
		_peersMutex.lock();
		if(_peersBySerial.find(serialNumber) != _peersBySerial.end())
		{
			std::shared_ptr<HMWiredPeer> peer(std::dynamic_pointer_cast<HMWiredPeer>(_peersBySerial.at(serialNumber)));
			_peersMutex.unlock();
			return peer;
		}
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    _peersMutex.unlock();
    return std::shared_ptr<HMWiredPeer>();
}

bool HMWiredCentral::onPacketReceived(std::string& senderID, std::shared_ptr<BaseLib::Systems::Packet> packet)
{
	try
	{
		if(_disposing) return false;
		std::shared_ptr<HMWiredPacket> hmWiredPacket(std::dynamic_pointer_cast<HMWiredPacket>(packet));
		if(!hmWiredPacket) return false;
		if(GD::bl->debugLevel >= 4) std::cout << BaseLib::HelperFunctions::getTimeString(hmWiredPacket->getTimeReceived()) << " HomeMatic Wired packet received: " + hmWiredPacket->hexString() << std::endl;
		_receivedPackets.set(hmWiredPacket->senderAddress(), hmWiredPacket, hmWiredPacket->getTimeReceived());
		std::shared_ptr<HMWiredPeer> peer(getPeer(hmWiredPacket->senderAddress()));
		if(peer) peer->packetReceived(hmWiredPacket);
		else if(hmWiredPacket->messageType() == 0x41 && !_pairing)
		{
			_announceThreadMutex.lock();
			_bl->threadManager.join(_announceThread);
			_bl->threadManager.start(_announceThread, false, &HMWiredCentral::handleAnnounce, this, hmWiredPacket);
			_announceThreadMutex.unlock();
		}
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    return false;
}

void HMWiredCentral::deletePeer(uint64_t id)
{
	try
	{
		std::shared_ptr<HMWiredPeer> peer(getPeer(id));
		if(!peer) return;
		peer->deleting = true;
		PVariable deviceAddresses(new Variable(VariableType::tArray));
		deviceAddresses->arrayValue->push_back(PVariable(new Variable(peer->getSerialNumber())));
		std::shared_ptr<HomegearDevice> rpcDevice = peer->getRpcDevice();
		for(Functions::iterator i = rpcDevice->functions.begin(); i != rpcDevice->functions.end(); ++i)
		{
			deviceAddresses->arrayValue->push_back(PVariable(new Variable(peer->getSerialNumber() + ":" + std::to_string(i->first))));
		}
		PVariable deviceInfo(new Variable(VariableType::tStruct));
		deviceInfo->structValue->insert(StructElement("ID", PVariable(new Variable((int32_t)peer->getID()))));
		PVariable channels(new Variable(VariableType::tArray));
		deviceInfo->structValue->insert(StructElement("CHANNELS", channels));
		for(Functions::iterator i = rpcDevice->functions.begin(); i != rpcDevice->functions.end(); ++i)
		{
			channels->arrayValue->push_back(PVariable(new Variable(i->first)));
		}

        std::vector<uint64_t> deletedIds{ id };
		raiseRPCDeleteDevices(deletedIds, deviceAddresses, deviceInfo);

        {
            std::lock_guard<std::mutex> peersGuard(_peersMutex);
            if(_peersBySerial.find(peer->getSerialNumber()) != _peersBySerial.end()) _peersBySerial.erase(peer->getSerialNumber());
            if(_peers.find(peer->getAddress()) != _peers.end()) _peers.erase(peer->getAddress());
            if(_peersById.find(id) != _peersById.end()) _peersById.erase(id);
        }

        int32_t i = 0;
        while(peer.use_count() > 1 && i < 600)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            i++;
        }
        if(i == 600) GD::out.printError("Error: Peer deletion took too long.");

		peer->deleteFromDatabase();

		GD::out.printMessage("Removed HomeMatic Wired peer " + std::to_string(peer->getID()));
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

std::shared_ptr<HMWiredPacket> HMWiredCentral::sendPacket(std::shared_ptr<HMWiredPacket> packet, bool resend, bool systemResponse)
{
	try
	{
		//First check if communication is in progress
		int64_t time = BaseLib::HelperFunctions::getTime();
		uint32_t busWaitingTime = GD::physicalInterface->getBusWaitingTime();
		std::shared_ptr<HMWiredPacketInfo> rxPacketInfo;
		std::shared_ptr<HMWiredPacketInfo> txPacketInfo = _sentPackets.getInfo(packet->destinationAddress());
		int64_t timeDifference = 0;
		if(txPacketInfo) timeDifference = time - txPacketInfo->time;
		//ACKs should always be sent immediately
		if(packet->type() != HMWiredPacketType::ackMessage && !GD::physicalInterface->autoResend() && (!txPacketInfo || timeDifference > 210))
		{
			rxPacketInfo = _receivedPackets.getInfo(packet->destinationAddress());
			int64_t rxTimeDifference = 0;
			if(rxPacketInfo) rxTimeDifference = time - rxPacketInfo->time;
			if(!rxPacketInfo || rxTimeDifference > 50)
			{
				//Communication might be in progress. Wait a little
				if(_bl->debugLevel > 4 && (time - GD::physicalInterface->lastPacketSent() < 210 || time - GD::physicalInterface->lastPacketReceived() < 210)) GD::out.printDebug("Debug: HomeMatic Wired Device 0x" + BaseLib::HelperFunctions::getHexString(_deviceId) + ": Waiting for RS485 bus to become free... (Packet: " + packet->hexString() + ")");
				if(GD::physicalInterface->getFastSending())
				{
					while(time - GD::physicalInterface->lastPacketSent() < busWaitingTime || time - GD::physicalInterface->lastPacketReceived() < busWaitingTime)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(20));
						time = BaseLib::HelperFunctions::getTime();
						if(time - GD::physicalInterface->lastPacketSent() >= busWaitingTime && time - GD::physicalInterface->lastPacketReceived() >= busWaitingTime)
						{
							int32_t sleepingTime = BaseLib::HelperFunctions::getRandomNumber(0, busWaitingTime / 2);
							if(_bl->debugLevel > 4) GD::out.printDebug("Debug: HomeMatic Wired Device 0x" + BaseLib::HelperFunctions::getHexString(_deviceId) + ": RS485 bus is free now. Waiting randomly for " + std::to_string(sleepingTime) + "ms... (Packet: " + packet->hexString() + ")");
							//Sleep random time
							std::this_thread::sleep_for(std::chrono::milliseconds(sleepingTime));
							time = BaseLib::HelperFunctions::getTime();
						}
					}
				}
				else
				{
					while(time - GD::physicalInterface->lastPacketSent() < 210 || time - GD::physicalInterface->lastPacketReceived() < 210)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
						time = BaseLib::HelperFunctions::getTime();
						if(time - GD::physicalInterface->lastPacketSent() >= 210 && time - GD::physicalInterface->lastPacketReceived() >= 210)
						{
							int32_t sleepingTime = BaseLib::HelperFunctions::getRandomNumber(0, 100);
							if(_bl->debugLevel > 4) GD::out.printDebug("Debug: HomeMatic Wired Device 0x" + BaseLib::HelperFunctions::getHexString(_deviceId) + ": RS485 bus is free now. Waiting randomly for " + std::to_string(sleepingTime) + "ms... (Packet: " + packet->hexString() + ")");
							//Sleep random time
							std::this_thread::sleep_for(std::chrono::milliseconds(sleepingTime));
							time = BaseLib::HelperFunctions::getTime();
						}
					}
				}
				if(_bl->debugLevel > 4) GD::out.printDebug("Debug: HomeMatic Wired Device 0x" + BaseLib::HelperFunctions::getHexString(_deviceId) + ": RS485 bus is still free... sending... (Packet: " + packet->hexString() + ")");
			}
		}
		//RS485 bus should be free
		uint32_t responseDelay = GD::physicalInterface->responseDelay();
		_sentPackets.set(packet->destinationAddress(), packet);
		if(txPacketInfo)
		{
			timeDifference = time - txPacketInfo->time;
			if(timeDifference < responseDelay)
			{
				txPacketInfo->time += responseDelay - timeDifference; //Set to sending time
				std::this_thread::sleep_for(std::chrono::milliseconds(responseDelay - timeDifference));
				time = BaseLib::HelperFunctions::getTime();
			}
		}
		rxPacketInfo = _receivedPackets.getInfo(packet->destinationAddress());
		if(rxPacketInfo)
		{
			int64_t timeDifference = time - rxPacketInfo->time;
			if(timeDifference >= 0 && timeDifference < responseDelay)
			{
				int64_t sleepingTime = responseDelay - timeDifference;
				if(sleepingTime > 1) sleepingTime -= 1;
				packet->setTimeSending(time + sleepingTime + 1);
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepingTime));
				time = BaseLib::HelperFunctions::getTime();
			}
			//Set time to now. This is necessary if two packets are sent after each other without a response in between
			rxPacketInfo->time = time;
		}
		else if(_bl->debugLevel > 4) GD::out.printDebug("Debug: Sending HomeMatic Wired packet " + packet->hexString() + " immediately, because it seems it is no response (no packet information found).", 7);

		std::shared_ptr<HMWiredPacket> receivedPacket;
		if(!GD::physicalInterface->autoResend() && resend)
		{
			if(GD::physicalInterface->getFastSending())
			{
				for(int32_t retries = 0; retries < 3; retries++)
				{
					int64_t time = BaseLib::HelperFunctions::getTime();
					std::chrono::milliseconds sleepingTime(5);
					if(retries > 0) _sentPackets.keepAlive(packet->destinationAddress());
					GD::physicalInterface->sendPacket(packet);
					if(packet->type() == HMWiredPacketType::ackMessage) return std::shared_ptr<HMWiredPacket>();
					for(int32_t i = 0; i < ((signed)busWaitingTime - 20) / 5; i++)
					{
						std::this_thread::sleep_for(sleepingTime);
						receivedPacket = systemResponse ? _receivedPackets.get(0) : _receivedPackets.get(packet->destinationAddress());
						if(receivedPacket && receivedPacket->getTimeReceived() >= time && receivedPacket->receiverMessageCounter() == packet->senderMessageCounter())
						{
							return receivedPacket;
						}
					}
				}
			}
			else
			{
				for(int32_t retries = 0; retries < 3; retries++)
				{
					int64_t time = BaseLib::HelperFunctions::getTime();
					std::chrono::milliseconds sleepingTime(5);
					if(retries > 0) _sentPackets.keepAlive(packet->destinationAddress());
					GD::physicalInterface->sendPacket(packet);
					if(packet->type() == HMWiredPacketType::ackMessage) return std::shared_ptr<HMWiredPacket>();
					for(int32_t i = 0; i < 8; i++)
					{
						if(i == 5) sleepingTime = std::chrono::milliseconds(25);
						std::this_thread::sleep_for(sleepingTime);
						receivedPacket = systemResponse ? _receivedPackets.get(0) : _receivedPackets.get(packet->destinationAddress());
						if(receivedPacket && receivedPacket->getTimeReceived() >= time && receivedPacket->receiverMessageCounter() == packet->senderMessageCounter())
						{
							return receivedPacket;
						}
					}
				}
			}
			std::shared_ptr<HMWiredPeer> peer = getPeer(packet->destinationAddress());
			if(peer) peer->serviceMessages->setUnreach(true, false);
		}
		else
		{
			int64_t time = BaseLib::HelperFunctions::getTime();
			std::chrono::milliseconds sleepingTime(5);
			GD::physicalInterface->sendPacket(packet);
			if(packet->type() == HMWiredPacketType::ackMessage) return std::shared_ptr<HMWiredPacket>();
			for(int32_t i = 0; i < 12; i++)
			{
				if(i == 5) sleepingTime = std::chrono::milliseconds(25);
				std::this_thread::sleep_for(sleepingTime);
				receivedPacket = systemResponse ? _receivedPackets.get(0) : _receivedPackets.get(packet->destinationAddress());
				if(receivedPacket && receivedPacket->getTimeReceived() >= time && receivedPacket->receiverMessageCounter() == packet->senderMessageCounter())
				{
					return receivedPacket;
				}
			}
		}
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    return std::shared_ptr<HMWiredPacket>();
}

void HMWiredCentral::lockBus()
{
	try
	{
		std::vector<uint8_t> payload = { 0x7A };
		std::shared_ptr<HMWiredPacket> packet(new HMWiredPacket(HMWiredPacketType::iMessage, _address, 0xFFFFFFFF, true, _messageCounter[0]++, 0, 0, payload));
		sendPacket(packet, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		packet.reset(new HMWiredPacket(HMWiredPacketType::iMessage, _address, 0xFFFFFFFF, true, _messageCounter[0]++, 0, 0, payload));
		sendPacket(packet, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void HMWiredCentral::unlockBus()
{
	try
	{
		std::vector<uint8_t> payload = { 0x5A };
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		std::shared_ptr<HMWiredPacket> packet(new HMWiredPacket(HMWiredPacketType::iMessage, _address, 0xFFFFFFFF, true, _messageCounter[0]++, 0, 0, payload));
		sendPacket(packet, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		packet.reset(new HMWiredPacket(HMWiredPacketType::iMessage, _address, 0xFFFFFFFF, true, _messageCounter[0]++, 0, 0, payload));
		sendPacket(packet, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

uint8_t HMWiredCentral::getMessageCounter(int32_t destinationAddress)
{
	try
	{
		std::shared_ptr<HMWiredPeer> peer = getPeer(destinationAddress);
		uint8_t messageCounter = 0;
		if(peer)
		{
			messageCounter = peer->getMessageCounter();
			peer->setMessageCounter(messageCounter + 1);
		}
		else messageCounter = _messageCounter[destinationAddress]++;
		return messageCounter;
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	return 0;
}

std::shared_ptr<HMWiredPacket> HMWiredCentral::getResponse(uint8_t command, int32_t destinationAddress, bool synchronizationBit)
{
	try
	{
		std::vector<uint8_t> payload({command});
		return getResponse(payload, destinationAddress, synchronizationBit);
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	return std::shared_ptr<HMWiredPacket>();
}

std::shared_ptr<HMWiredPacket> HMWiredCentral::getResponse(std::vector<uint8_t>& payload, int32_t destinationAddress, bool synchronizationBit)
{
	std::shared_ptr<HMWiredPeer> peer = getPeer(destinationAddress);
	try
	{
		if(peer) peer->ignorePackets = true;
		std::shared_ptr<HMWiredPacket> request(new HMWiredPacket(HMWiredPacketType::iMessage, _address, destinationAddress, synchronizationBit, getMessageCounter(destinationAddress), 0, 0, payload));
		std::shared_ptr<HMWiredPacket> response = sendPacket(request, true);
		if(response && response->type() != HMWiredPacketType::ackMessage) sendOK(response->senderMessageCounter(), destinationAddress);
		if(peer) peer->ignorePackets = false;
		return response;
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	if(peer) peer->ignorePackets = false;
	return std::shared_ptr<HMWiredPacket>();
}

std::shared_ptr<HMWiredPacket> HMWiredCentral::getResponse(std::shared_ptr<HMWiredPacket> packet, bool systemResponse)
{
	std::shared_ptr<HMWiredPeer> peer = getPeer(packet->destinationAddress());
	try
	{
		if(peer) peer->ignorePackets = true;
		std::shared_ptr<HMWiredPacket> request(packet);
		std::shared_ptr<HMWiredPacket> response = sendPacket(request, true, systemResponse);
		if(response && response->type() != HMWiredPacketType::ackMessage && response->type() != HMWiredPacketType::system) sendOK(response->senderMessageCounter(), packet->destinationAddress());
		if(peer) peer->ignorePackets = false;
		return response;
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	if(peer) peer->ignorePackets = false;
	return std::shared_ptr<HMWiredPacket>();
}

std::vector<uint8_t> HMWiredCentral::readEEPROM(int32_t deviceAddress, int32_t eepromAddress)
{
	std::shared_ptr<HMWiredPeer> peer = getPeer(deviceAddress);
	try
	{
		if(peer) peer->ignorePackets = true;
		std::vector<uint8_t> payload;
		payload.push_back(0x52); //Command read EEPROM
		payload.push_back(eepromAddress >> 8);
		payload.push_back(eepromAddress & 0xFF);
		payload.push_back(0x10); //Bytes to read
		std::shared_ptr<HMWiredPacket> request(new HMWiredPacket(HMWiredPacketType::iMessage, _address, deviceAddress, false, getMessageCounter(deviceAddress), 0, 0, payload));
		std::shared_ptr<HMWiredPacket> response = sendPacket(request, true);
		if(response)
		{
			sendOK(response->senderMessageCounter(), deviceAddress);
			if(peer) peer->ignorePackets = false;
			return response->payload();
		}
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	if(peer) peer->ignorePackets = false;
	return std::vector<uint8_t>();
}

bool HMWiredCentral::writeEEPROM(int32_t deviceAddress, int32_t eepromAddress, std::vector<uint8_t>& data)
{
	std::shared_ptr<HMWiredPeer> peer = getPeer(deviceAddress);
	try
	{
		if(data.size() > 32)
		{
			GD::out.printError("Error: HomeMatic Wired Device " + std::to_string(_deviceId) + ": Could not write data to EEPROM. Data size is larger than 32 bytes.");
			return false;
		}
		if(peer) peer->ignorePackets = true;
		std::vector<uint8_t> payload;
		payload.push_back(0x57); //Command write EEPROM
		payload.push_back(eepromAddress >> 8);
		payload.push_back(eepromAddress & 0xFF);
		payload.push_back(data.size()); //Bytes to write
		payload.insert(payload.end(), data.begin(), data.end());
		std::shared_ptr<HMWiredPacket> request(new HMWiredPacket(HMWiredPacketType::iMessage, _address, deviceAddress, false, getMessageCounter(deviceAddress), 0, 0, payload));
		std::shared_ptr<HMWiredPacket> response = sendPacket(request, true);
		if(response)
		{
			if(peer) peer->ignorePackets = false;
			return true;
		}
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	if(peer) peer->ignorePackets = false;
	return false;
}

void HMWiredCentral::sendOK(int32_t messageCounter, int32_t destinationAddress)
{
	try
	{
		std::vector<uint8_t> payload;
		std::shared_ptr<HMWiredPacket> ackPacket(new HMWiredPacket(HMWiredPacketType::ackMessage, _address, destinationAddress, false, 0, messageCounter, 0, payload));
		sendPacket(ackPacket, false);
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
}

void HMWiredCentral::saveMessageCounters()
{
	try
	{
		std::vector<uint8_t> serializedData;
		serializeMessageCounters(serializedData);
		saveVariable(2, serializedData);
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void HMWiredCentral::serializeMessageCounters(std::vector<uint8_t>& encodedData)
{
	try
	{
		BaseLib::BinaryEncoder encoder(_bl);
		encoder.encodeInteger(encodedData, _messageCounter.size());
		for(std::unordered_map<int32_t, uint8_t>::const_iterator i = _messageCounter.begin(); i != _messageCounter.end(); ++i)
		{
			encoder.encodeInteger(encodedData, i->first);
			encoder.encodeByte(encodedData, i->second);
		}
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void HMWiredCentral::unserializeMessageCounters(std::shared_ptr<std::vector<char>> serializedData)
{
	try
	{
		BaseLib::BinaryDecoder decoder(_bl);
		uint32_t position = 0;
		uint32_t messageCounterSize = decoder.decodeInteger(*serializedData, position);
		for(uint32_t i = 0; i < messageCounterSize; i++)
		{
			int32_t index = decoder.decodeInteger(*serializedData, position);
			_messageCounter[index] = decoder.decodeByte(*serializedData, position);
		}
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
}

void HMWiredCentral::savePeers(bool full)
{
	try
	{
		_peersMutex.lock();
		for(std::unordered_map<int32_t, std::shared_ptr<BaseLib::Systems::Peer>>::iterator i = _peers.begin(); i != _peers.end(); ++i)
		{
			//Necessary, because peers can be assigned to multiple virtual devices
			if(i->second->getParentID() != _deviceId) continue;
			//We are always printing this, because the init script needs it
			GD::out.printMessage("(Shutdown) => Saving HomeMatic Wired peer " + std::to_string(i->second->getID()));
			i->second->save(full, full, full);
		}
	}
	catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
	_peersMutex.unlock();
}

std::string HMWiredCentral::handleCliCommand(std::string command)
{
	try
	{
		std::ostringstream stringStream;
		if(command == "help" || command == "h")
		{
			stringStream << "List of commands:" << std::endl << std::endl;
			stringStream << "For more information about the individual command type: COMMAND help" << std::endl << std::endl;
			stringStream << "peers list (ls)\t\tList all peers" << std::endl;
			stringStream << "peers reset (prs)\tUnpair a peer and reset it to factory defaults" << std::endl;
			stringStream << "peers select (ps)\tSelect a peer" << std::endl;
			stringStream << "peers setname (pn)\tName a peer" << std::endl;
			stringStream << "peers unpair (pup)\tUnpair a peer" << std::endl;
			stringStream << "peers update (pud)\tUpdates a peer to the newest firmware version" << std::endl;
			stringStream << "search (sp)\t\tSearches for new devices on the bus" << std::endl;
			stringStream << "unselect (u)\t\tUnselect this device" << std::endl;
			return stringStream.str();
		}
		if(command.compare(0, 6, "search") == 0 || command.compare(0, 2, "sp") == 0)
		{
			std::stringstream stream(command);
			std::string element;
			int32_t offset = (command.at(1) == 'p') ? 0 : 1;
			int32_t index = 0;
			while(std::getline(stream, element, ' '))
			{
				if(index < 1 + offset)
				{
					index++;
					continue;
				}
				else if(index == 1 + offset)
				{
					if(element == "help")
					{
						stringStream << "Description: This command searches for new devices on the bus." << std::endl;
						stringStream << "Usage: search" << std::endl << std::endl;
						stringStream << "Parameters:" << std::endl;
						stringStream << "  There are no parameters." << std::endl;
						return stringStream.str();
					}
				}
				index++;
			}

			PVariable result = searchDevices(nullptr, "");
			if(result->errorStruct) stringStream << "Error: " << result->structValue->at("faultString")->stringValue << std::endl;
			else stringStream << "Search completed successfully." << std::endl;
			return stringStream.str();
		}
		else if(command.compare(0, 12, "peers unpair") == 0 || command.compare(0, 3, "pup") == 0)
		{
			uint64_t peerID = 0;

			std::stringstream stream(command);
			std::string element;
			int32_t offset = (command.at(1) == 'u') ? 0 : 1;
			int32_t index = 0;
			while(std::getline(stream, element, ' '))
			{
				if(index < 1 + offset)
				{
					index++;
					continue;
				}
				else if(index == 1 + offset)
				{
					if(element == "help") break;
					peerID = BaseLib::Math::getNumber(element, false);
					if(peerID == 0) return "Invalid id.\n";
				}
				index++;
			}
			if(index == 1 + offset)
			{
				stringStream << "Description: This command unpairs a peer." << std::endl;
				stringStream << "Usage: peers unpair PEERID" << std::endl << std::endl;
				stringStream << "Parameters:" << std::endl;
				stringStream << "  PEERID:\tThe id of the peer to unpair. Example: 513" << std::endl;
				return stringStream.str();
			}

			if(!peerExists(peerID)) stringStream << "This peer is not paired to this central." << std::endl;
			else
			{
				stringStream << "Unpairing peer " << std::to_string(peerID) << std::endl;
				deletePeer(peerID);
			}
			return stringStream.str();
		}
		else if(command.compare(0, 11, "peers reset") == 0 || command.compare(0, 3, "prs") == 0)
		{
			uint64_t peerID = 0;

			std::stringstream stream(command);
			std::string element;
			int32_t offset = (command.at(1) == 'r') ? 0 : 1;
			int32_t index = 0;
			while(std::getline(stream, element, ' '))
			{
				if(index < 1 + offset)
				{
					index++;
					continue;
				}
				else if(index == 1 + offset)
				{
					if(element == "help") break;
					peerID = BaseLib::Math::getNumber(element, false);
					if(peerID == 0) return "Invalid id.\n";
				}
				index++;
			}
			if(index == 1 + offset)
			{
				stringStream << "Description: This command resets and unpairs a peer." << std::endl;
				stringStream << "Usage: peers reset PEERID" << std::endl << std::endl;
				stringStream << "Parameters:" << std::endl;
				stringStream << "  PEERID:\tThe id of the peer to reset. Example: 513" << std::endl;
				return stringStream.str();
			}

			std::shared_ptr<HMWiredPeer> peer = getPeer(peerID);
			if(!peer) stringStream << "This peer is not paired to this central." << std::endl;
			else
			{
				stringStream << "Resetting peer " << std::to_string(peerID) << std::endl;
				peer->reset();
				deletePeer(peerID);
			}
			return stringStream.str();
		}
		else if(command.compare(0, 10, "peers list") == 0 || command.compare(0, 2, "pl") == 0 || command.compare(0, 2, "ls") == 0)
		{
			try
			{
				std::string filterType;
				std::string filterValue;

				std::stringstream stream(command);
				std::string element;
				int32_t offset = (command.at(1) == 'l' || command.at(1) == 's') ? 0 : 1;
				int32_t index = 0;
				while(std::getline(stream, element, ' '))
				{
					if(index < 1 + offset)
					{
						index++;
						continue;
					}
					else if(index == 1 + offset)
					{
						if(element == "help")
						{
							index = -1;
							break;
						}
						filterType = BaseLib::HelperFunctions::toLower(element);
					}
					else if(index == 2 + offset)
					{
						filterValue = element;
						if(filterType == "name") BaseLib::HelperFunctions::toLower(filterValue);
					}
					index++;
				}
				if(index == -1)
				{
					stringStream << "Description: This command lists information about all peers." << std::endl;
					stringStream << "Usage: peers list [FILTERTYPE] [FILTERVALUE]" << std::endl << std::endl;
					stringStream << "Parameters:" << std::endl;
					stringStream << "  FILTERTYPE:\tSee filter types below." << std::endl;
					stringStream << "  FILTERVALUE:\tDepends on the filter type. If a number is required, it has to be in hexadecimal format." << std::endl << std::endl;
					stringStream << "Filter types:" << std::endl;
					stringStream << "  ID: Filter by id." << std::endl;
					stringStream << "      FILTERVALUE: The id of the peer to filter (e. g. 513)." << std::endl;
					stringStream << "  ADDRESS: Filter by address." << std::endl;
					stringStream << "      FILTERVALUE: The 4 byte address of the peer to filter (e. g. 001DA44D)." << std::endl;
					stringStream << "  SERIAL: Filter by serial number." << std::endl;
					stringStream << "      FILTERVALUE: The serial number of the peer to filter (e. g. JEQ0554309)." << std::endl;
					stringStream << "  NAME: Filter by name." << std::endl;
					stringStream << "      FILTERVALUE: The part of the name to search for (e. g. \"1st floor\")." << std::endl;
					stringStream << "  TYPE: Filter by device type." << std::endl;
					stringStream << "      FILTERVALUE: The 2 byte device type in hexadecimal format." << std::endl;
					stringStream << "  UNREACH: List all unreachable peers." << std::endl;
					stringStream << "      FILTERVALUE: empty" << std::endl;
					return stringStream.str();
				}

				if(_peers.empty())
				{
					stringStream << "No peers are paired to this central." << std::endl;
					return stringStream.str();
				}
				bool firmwareUpdates = false;
				std::string bar(" │ ");
				const int32_t idWidth = 8;
				const int32_t nameWidth = 25;
				const int32_t addressWidth = 8;
				const int32_t serialWidth = 13;
				const int32_t typeWidth1 = 4;
				const int32_t typeWidth2 = 25;
				const int32_t firmwareWidth = 8;
				const int32_t unreachWidth = 7;
				std::string nameHeader("Name");
				nameHeader.resize(nameWidth, ' ');
				std::string typeStringHeader("Type String");
				typeStringHeader.resize(typeWidth2, ' ');
				stringStream << std::setfill(' ')
					<< std::setw(idWidth) << "ID" << bar
					<< nameHeader << bar
					<< std::setw(addressWidth) << "Address" << bar
					<< std::setw(serialWidth) << "Serial Number" << bar
					<< std::setw(typeWidth1) << "Type" << bar
					<< typeStringHeader << bar
					<< std::setw(firmwareWidth) << "Firmware" << bar
					<< std::setw(unreachWidth) << "Unreach"
					<< std::endl;
				stringStream << "─────────┼───────────────────────────┼──────────┼───────────────┼──────┼───────────────────────────┼──────────┼────────" << std::endl;
				stringStream << std::setfill(' ')
					<< std::setw(idWidth) << " " << bar
					<< std::setw(nameWidth) << " " << bar
					<< std::setw(addressWidth) << " " << bar
					<< std::setw(serialWidth) << " " << bar
					<< std::setw(typeWidth1) << " " << bar
					<< std::setw(typeWidth2) << " " << bar
					<< std::setw(firmwareWidth) << " " << bar
					<< std::setw(unreachWidth) << " "
					<< std::endl;
				_peersMutex.lock();
				for(std::map<uint64_t, std::shared_ptr<BaseLib::Systems::Peer>>::iterator i = _peersById.begin(); i != _peersById.end(); ++i)
				{
					if(filterType == "id")
					{
						uint64_t id = BaseLib::Math::getNumber(filterValue, false);
						if(i->second->getID() != id) continue;
					}
					else if(filterType == "name")
					{
						std::string name = i->second->getName();
						if((signed)BaseLib::HelperFunctions::toLower(name).find(filterValue) == (signed)std::string::npos) continue;
					}
					else if(filterType == "address")
					{
						int32_t address = BaseLib::Math::getNumber(filterValue, true);
						if(i->second->getAddress() != address) continue;
					}
					else if(filterType == "serial")
					{
						if(i->second->getSerialNumber() != filterValue) continue;
					}
					else if(filterType == "type")
					{
						int32_t deviceType = BaseLib::Math::getNumber(filterValue, true);
						if((int32_t)i->second->getDeviceType() != deviceType) continue;
					}
					else if(filterType == "unreach")
					{
						if(i->second->serviceMessages)
						{
							if(!i->second->serviceMessages->getUnreach()) continue;
						}
					}

					stringStream << std::setw(idWidth) << std::setfill(' ') << std::to_string(i->second->getID()) << bar;
					std::string name = i->second->getName();
					size_t nameSize = BaseLib::HelperFunctions::utf8StringSize(name);
					if(nameSize > (unsigned)nameWidth)
					{
						name = BaseLib::HelperFunctions::utf8Substring(name, 0, nameWidth - 3);
						name += "...";
					}
					else name.resize(nameWidth + (name.size() - nameSize), ' ');
					stringStream << name << bar
						<< std::setw(addressWidth) << BaseLib::HelperFunctions::getHexString(i->second->getAddress(), 8) << bar
						<< std::setw(serialWidth) << i->second->getSerialNumber() << bar
						<< std::setw(typeWidth1) << BaseLib::HelperFunctions::getHexString(i->second->getDeviceType(), 4) << bar;
					if(i->second->getRpcDevice())
					{
						PSupportedDevice type = i->second->getRpcDevice()->getType(i->second->getDeviceType(), i->second->getFirmwareVersion());
						std::string typeID;
						if(type) typeID = type->id;
						if(typeID.size() > (unsigned)typeWidth2)
						{
							typeID.resize(typeWidth2 - 3);
							typeID += "...";
						}
						else typeID.resize(typeWidth2, ' ');
						stringStream << typeID << bar;
					}
					else stringStream << std::setw(typeWidth2) << " " << bar;
					if(i->second->getFirmwareVersion() == 0) stringStream << std::setfill(' ') << std::setw(firmwareWidth) << "?" << bar;
					else if(i->second->firmwareUpdateAvailable())
					{
						stringStream << std::setfill(' ') << std::setw(firmwareWidth) << ("*" + BaseLib::HelperFunctions::getHexString(i->second->getFirmwareVersion() >> 8) + "." + std::to_string(i->second->getFirmwareVersion() & 0xFF)) << bar;
						firmwareUpdates = true;
					}
					else stringStream << std::setfill(' ') << std::setw(firmwareWidth) << (BaseLib::HelperFunctions::getHexString(i->second->getFirmwareVersion() >> 8) + "." + std::to_string(i->second->getFirmwareVersion() & 0xFF)) << bar;
					if(i->second->serviceMessages)
					{
						std::string unreachable(i->second->serviceMessages->getUnreach() ? "Yes" : "No");
						stringStream << std::setw(unreachWidth) << unreachable;
					}
					stringStream << std::endl << std::dec;
				}
				_peersMutex.unlock();
				stringStream << "─────────┴───────────────────────────┴──────────┴───────────────┴──────┴───────────────────────────┴──────────┴────────" << std::endl;
				if(firmwareUpdates) stringStream << std::endl << "*: Firmware update available." << std::endl;

				return stringStream.str();
			}
			catch(const std::exception& ex)
			{
				_peersMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
			}
			catch(...)
			{
				_peersMutex.unlock();
				GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
			}
		}
		else if(command.compare(0, 13, "peers setname") == 0 || command.compare(0, 2, "pn") == 0)
		{
			uint64_t peerID = 0;
			std::string name;

			std::stringstream stream(command);
			std::string element;
			int32_t offset = (command.at(1) == 'n') ? 0 : 1;
			int32_t index = 0;
			while(std::getline(stream, element, ' '))
			{
				if(index < 1 + offset)
				{
					index++;
					continue;
				}
				else if(index == 1 + offset)
				{
					if(element == "help") break;
					else
					{
						peerID = BaseLib::Math::getNumber(element, false);
						if(peerID == 0) return "Invalid id.\n";
					}
				}
				else if(index == 2 + offset) name = element;
				else name += ' ' + element;
				index++;
			}
			if(index == 1 + offset)
			{
				stringStream << "Description: This command sets or changes the name of a peer to identify it more easily." << std::endl;
				stringStream << "Usage: peers setname PEERID NAME" << std::endl << std::endl;
				stringStream << "Parameters:" << std::endl;
				stringStream << "  PEERID:\tThe id of the peer to set the name for. Example: 513" << std::endl;
				stringStream << "  NAME:\tThe name to set. Example: \"1st floor light switch\"." << std::endl;
				return stringStream.str();
			}

			if(!peerExists(peerID)) stringStream << "This peer is not paired to this central." << std::endl;
			else
			{
				std::shared_ptr<HMWiredPeer> peer = getPeer(peerID);
				peer->setName(name);
				stringStream << "Name set to \"" << name << "\"." << std::endl;
			}
			return stringStream.str();
		}
		else if(command.compare(0, 12, "peers update") == 0 || command.compare(0, 3, "pud") == 0)
		{
			uint64_t peerID;
			bool all = false;

			std::stringstream stream(command);
			std::string element;
			int32_t offset = (command.at(1) == 'u') ? 0 : 1;
			int32_t index = 0;
			while(std::getline(stream, element, ' '))
			{
				if(index < 1 + offset)
				{
					index++;
					continue;
				}
				else if(index == 1 + offset)
				{
					if(element == "help") break;
					else if(element == "all") all = true;
					else
					{
						peerID = BaseLib::Math::getNumber(element, false);
						if(peerID == 0) return "Invalid id.\n";
					}
				}
				index++;
			}
			if(index == 1 + offset)
			{
				stringStream << "Description: This command updates one or all peers to the newest firmware version available in \"" << _bl->settings.firmwarePath() << "\"." << std::endl;
				stringStream << "Usage: peers update PEERID" << std::endl;
				stringStream << "       peers update all" << std::endl << std::endl;
				stringStream << "Parameters:" << std::endl;
				stringStream << "  PEERID:\tThe id of the peer to update. Example: 513" << std::endl;
				return stringStream.str();
			}

			PVariable result;
			std::vector<uint64_t> ids;
			if(all)
			{
				_peersMutex.lock();
				for(std::map<uint64_t, std::shared_ptr<BaseLib::Systems::Peer>>::iterator i = _peersById.begin(); i != _peersById.end(); ++i)
				{
					if(i->second->firmwareUpdateAvailable()) ids.push_back(i->first);
				}
				_peersMutex.unlock();
				if(ids.empty())
				{
					stringStream << "All peers are up to date." << std::endl;
					return stringStream.str();
				}
				result = updateFirmware(nullptr, ids, false);
			}
			else if(!peerExists(peerID)) stringStream << "This peer is not paired to this central." << std::endl;
			else
			{
				std::shared_ptr<HMWiredPeer> peer = getPeer(peerID);
				if(!peer->firmwareUpdateAvailable())
				{
					stringStream << "Peer is up to date." << std::endl;
					return stringStream.str();
				}
				ids.push_back(peerID);
				result = updateFirmware(nullptr, ids, false);
			}
			if(!result) stringStream << "Unknown error." << std::endl;
			else if(result->errorStruct) stringStream << result->structValue->at("faultString")->stringValue << std::endl;
			else stringStream << "Started firmware update(s)... This might take a long time. Use the RPC function \"getUpdateStatus\" or see the log for details." << std::endl;
			return stringStream.str();
		}
		else return "Unknown command.\n";
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    return "Error executing command. See log file for more details.\n";
}

std::shared_ptr<HMWiredPeer> HMWiredCentral::createPeer(int32_t address, int32_t firmwareVersion, uint32_t deviceType, std::string serialNumber, bool save)
{
	try
	{
		std::shared_ptr<HMWiredPeer> peer(new HMWiredPeer(_deviceId, this));
		peer->setAddress(address);
		peer->setFirmwareVersion(firmwareVersion);
		peer->setDeviceType(deviceType);
		peer->setSerialNumber(serialNumber);
		peer->setRpcDevice(GD::family->getRpcDevices()->find(deviceType, firmwareVersion, -1));
		if(!peer->getRpcDevice()) return std::shared_ptr<HMWiredPeer>();
		if(save) peer->save(true, true, false); //Save and create peerID
		return peer;
	}
    catch(const std::exception& ex)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
    	GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    return std::shared_ptr<HMWiredPeer>();
}

void HMWiredCentral::updateFirmwares(std::vector<uint64_t> ids)
{
	try
	{
		if(_updateMode || _bl->deviceUpdateInfo.currentDevice > 0) return;
		_bl->deviceUpdateInfo.updateMutex.lock();
		_bl->deviceUpdateInfo.devicesToUpdate = ids.size();
		_bl->deviceUpdateInfo.currentUpdate = 0;
		for(std::vector<uint64_t>::iterator i = ids.begin(); i != ids.end(); ++i)
		{
			_bl->deviceUpdateInfo.currentDeviceProgress = 0;
			_bl->deviceUpdateInfo.currentUpdate++;
			_bl->deviceUpdateInfo.currentDevice = *i;
			updateFirmware(*i);
		}
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
	_bl->deviceUpdateInfo.reset();
	_bl->deviceUpdateInfo.updateMutex.unlock();
}

void HMWiredCentral::updateFirmware(uint64_t id)
{
	try
	{
		if(_updateMode) return;
		std::shared_ptr<HMWiredPeer> peer = getPeer(id);
		if(!peer) return;
		_updateMode = true;
		_updateMutex.lock();
		std::string filenamePrefix = BaseLib::HelperFunctions::getHexString(1, 4) + "." + BaseLib::HelperFunctions::getHexString(peer->getDeviceType(), 8);
		std::string versionFile(_bl->settings.firmwarePath() + filenamePrefix + ".version");
		if(!BaseLib::Io::fileExists(versionFile))
		{
			GD::out.printInfo("Info: Not updating peer with id " + std::to_string(id) + ". No version info file found.");
			_bl->deviceUpdateInfo.results[id].first = 2;
			_bl->deviceUpdateInfo.results[id].second = "No version file found.";
			_updateMutex.unlock();
			_updateMode = false;
			return;
		}
		std::string firmwareFile(_bl->settings.firmwarePath() + filenamePrefix + ".fw");
		if(!BaseLib::Io::fileExists(firmwareFile))
		{
			GD::out.printInfo("Info: Not updating peer with id " + std::to_string(id) + ". No firmware file found.");
			_bl->deviceUpdateInfo.results[id].first = 3;
			_bl->deviceUpdateInfo.results[id].second = "No firmware file found.";
			_updateMutex.unlock();
			_updateMode = false;
			return;
		}
		int32_t firmwareVersion = peer->getNewFirmwareVersion();
		if(peer->getFirmwareVersion() >= firmwareVersion)
		{
			_bl->deviceUpdateInfo.results[id].first = 0;
			_bl->deviceUpdateInfo.results[id].second = "Already up to date.";
			GD::out.printInfo("Info: Not updating peer with id " + std::to_string(id) + ". Peer firmware is already up to date.");
			_updateMutex.unlock();
			_updateMode = false;
			return;
		}
		std::string oldVersionString = BaseLib::HelperFunctions::getHexString(peer->getFirmwareVersion() >> 8) + "." + BaseLib::HelperFunctions::getHexString(peer->getFirmwareVersion() & 0xFF, 2);
		std::string versionString = BaseLib::HelperFunctions::getHexString(firmwareVersion >> 8) + "." + BaseLib::HelperFunctions::getHexString(firmwareVersion & 0xFF, 2);

		std::string firmwareHex;
		try
		{
			firmwareHex = BaseLib::Io::getFileContent(firmwareFile);
		}
		catch(const std::exception& ex)
		{
			GD::out.printError("Error: Could not open firmware file: " + firmwareFile + ": " + ex.what());
			_bl->deviceUpdateInfo.results[id].first = 4;
			_bl->deviceUpdateInfo.results[id].second = "Could not open firmware file.";
			_updateMutex.unlock();
			_updateMode = false;
			return;
		}
		catch(...)
		{
			GD::out.printError("Error: Could not open firmware file: " + firmwareFile + ".");
			_bl->deviceUpdateInfo.results[id].first = 4;
			_bl->deviceUpdateInfo.results[id].second = "Could not open firmware file.";
			_updateMutex.unlock();
			_updateMode = false;
			return;
		}

		std::stringstream stream(firmwareHex);
		std::string line;
		int32_t currentAddress = 0;
		std::vector<uint8_t> firmware;
		while(std::getline(stream, line))
		{
			if(line.at(0) != ':' || line.size() < 11)
			{
				_bl->deviceUpdateInfo.results[id].first = 5;
				_bl->deviceUpdateInfo.results[id].second = "Firmware file has wrong format.";
				GD::out.printError("Error: Could not read firmware file: " + firmwareFile + ": Wrong format (no colon at position 0 or line too short).");
				_updateMutex.unlock();
				_updateMode = false;
				return;
			}
			std::string hex = line.substr(1, 2);
			int32_t bytes = BaseLib::Math::getNumber(hex, true);
			hex = line.substr(7, 2);
			int32_t recordType = BaseLib::Math::getNumber(hex, true);
			if(recordType == 1) break; //End of file
			if(recordType != 0)
			{
				_bl->deviceUpdateInfo.results[id].first = 5;
				_bl->deviceUpdateInfo.results[id].second = "Firmware file has wrong format.";
				GD::out.printError("Error: Could not read firmware file: " + firmwareFile + ": Wrong format (wrong record type).");
				_updateMutex.unlock();
				_updateMode = false;
				return;
			}
			hex = line.substr(3, 4);
			int32_t address = BaseLib::Math::getNumber(hex, true);
			if(address != currentAddress || (11 + bytes * 2) > (signed)line.size())
			{
				_bl->deviceUpdateInfo.results[id].first = 5;
				_bl->deviceUpdateInfo.results[id].second = "Firmware file has wrong format.";
				GD::out.printError("Error: Could not read firmware file: " + firmwareFile + ": Wrong format (address does not match).");
				_updateMutex.unlock();
				_updateMode = false;
				return;
			}
			currentAddress += bytes;
			std::vector<uint8_t> data = _bl->hf.getUBinary(line.substr(9, bytes * 2));
			hex = line.substr(9 + bytes * 2, 2);
			int32_t checkSum = BaseLib::Math::getNumber(hex, true);
			int32_t calculatedCheckSum = bytes + (address >> 8) + (address & 0xFF) + recordType;
			for(std::vector<uint8_t>::iterator i = data.begin(); i != data.end(); ++i)
			{
				calculatedCheckSum += *i;
			}
			calculatedCheckSum = (((calculatedCheckSum & 0xFF) ^ 0xFF) + 1) & 0xFF;
			if(calculatedCheckSum != checkSum)
			{
				_bl->deviceUpdateInfo.results[id].first = 5;
				_bl->deviceUpdateInfo.results[id].second = "Firmware file has wrong format.";
				GD::out.printError("Error: Could not read firmware file: " + firmwareFile + ": Wrong format (check sum failed).");
				_updateMutex.unlock();
				_updateMode = false;
				return;
			}
			firmware.insert(firmware.end(), data.begin(), data.end());
		}

		lockBus();

		std::shared_ptr<HMWiredPacket> response = getResponse(0x75, peer->getAddress(), true);
		if(!response || response->type() != HMWiredPacketType::ackMessage)
		{
			unlockBus();
			_bl->deviceUpdateInfo.results[id].first = 6;
			_bl->deviceUpdateInfo.results[id].second = "Device did not respond to enter-bootloader packet.";
			GD::out.printWarning("Warning: Device did not enter bootloader.");
			_updateMutex.unlock();
			_updateMode = false;
			return;
		}

		//Wait for the device to enter bootloader
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		std::vector<uint8_t> payload;
		payload.push_back(0x75);
		std::shared_ptr<HMWiredPacket> packet(new HMWiredPacket(HMWiredPacketType::iMessage, 0, peer->getAddress(), false, getMessageCounter(peer->getAddress()), 0, 0, payload));
		response = getResponse(packet, true);
		if(!response || response->type() != HMWiredPacketType::system)
		{
			unlockBus();
			_bl->deviceUpdateInfo.results[id].first = 6;
			_bl->deviceUpdateInfo.results[id].second = "Device did not respond to enter-bootloader packet.";
			GD::out.printWarning("Warning: Device did not enter bootloader.");
			_updateMutex.unlock();
			_updateMode = false;
			return;
		}

		payload.clear();
		payload.push_back(0x70);
		packet.reset(new HMWiredPacket(HMWiredPacketType::iMessage, 0, peer->getAddress(), false, getMessageCounter(peer->getAddress()), 0, 0, payload));
		response = getResponse(packet, true);
		int32_t packetSize = 0;
		if(response && response->payload().size() == 2) packetSize = (response->payload().at(0) << 8) + response->payload().at(1);
		if(!response || response->type() != HMWiredPacketType::system || response->payload().size() != 2 || packetSize > 128 || packetSize == 0)
		{
			unlockBus();
			_bl->deviceUpdateInfo.results[id].first = 8;
			_bl->deviceUpdateInfo.results[id].second = "Too many communication errors (block size request failed).";
			GD::out.printWarning("Error: Block size request failed.");
			_updateMutex.unlock();
			_updateMode = false;
			return;
		}

		std::vector<uint8_t> data;
		for(int32_t i = 0; i < (signed)firmware.size(); i += packetSize)
		{
			_bl->deviceUpdateInfo.currentDeviceProgress = (i * 100) / firmware.size();
			int32_t currentPacketSize = (i + packetSize < (signed)firmware.size()) ? packetSize : firmware.size() - i;
			data.clear();
			data.push_back(0x77); //Type
			data.push_back(i >> 8); //Address
			data.push_back(i & 0xFF); //Address
			data.push_back(currentPacketSize); //Length
			data.insert(data.end(), firmware.begin() + i, firmware.begin() + i + currentPacketSize);

			std::shared_ptr<HMWiredPacket> packet(new HMWiredPacket(HMWiredPacketType::iMessage, 0, peer->getAddress(), false, getMessageCounter(peer->getAddress()), 0, 0, data));
			response = getResponse(packet, true);
			if(!response || response->type() != HMWiredPacketType::system || response->payload().size() != 2)
			{
				unlockBus();
				_bl->deviceUpdateInfo.results[id].first = 8;
				_bl->deviceUpdateInfo.results[id].second = "Too many communication errors.";
				GD::out.printWarning("Error: Block size request failed.");
				_updateMutex.unlock();
				_updateMode = false;
				return;
			}
			int32_t receivedBytes = (response->payload().at(0) << 8) + response->payload().at(1);
			if(receivedBytes != currentPacketSize)
			{
				unlockBus();
				_bl->deviceUpdateInfo.results[id].first = 8;
				_bl->deviceUpdateInfo.results[id].second = "Too many communication errors (device received wrong number of bytes).";
				GD::out.printWarning("Error: Block size request failed.");
				_updateMutex.unlock();
				_updateMode = false;
				return;
			}
		}

		payload.clear();
		payload.push_back(0x67);
		packet.reset(new HMWiredPacket(HMWiredPacketType::iMessage, 0, peer->getAddress(), false, getMessageCounter(peer->getAddress()), 0, 0, payload));
		for(int32_t i = 0; i < 3; i++)
		{
			sendPacket(packet, false);
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}

		unlockBus();

		peer->setFirmwareVersion(firmwareVersion);
		_bl->deviceUpdateInfo.results[id].first = 0;
		_bl->deviceUpdateInfo.results[id].second = "Update successful.";
		GD::out.printInfo("Info: Peer " + std::to_string(id) + " was successfully updated to firmware version " + versionString + ".");
		_updateMutex.unlock();
		_updateMode = false;
		return;
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    unlockBus();
    _bl->deviceUpdateInfo.results[id].first = 1;
	_bl->deviceUpdateInfo.results[id].second = "Unknown error.";
    _updateMutex.unlock();
    _updateMode = false;
}

void HMWiredCentral::handleAnnounce(std::shared_ptr<HMWiredPacket> packet)
{
	try
	{
		_peerInitMutex.lock();
		if(getPeer(packet->senderAddress()))
		{
			_peerInitMutex.unlock();
			return;
		}
		GD::out.printInfo("Info: New device detected on bus.");
		if(packet->payload().size() != 16)
		{
			GD::out.printWarning("Warning: Could not interpret announce packet: Packet has unknown size (payload size has to be 16).");
			_peerInitMutex.unlock();
			return;
		}
		int32_t deviceType = (packet->payload().at(2) << 8) + packet->payload().at(3);
		int32_t firmwareVersion = (packet->payload().at(4) << 8) + packet->payload().at(5);
		std::string serialNumber((char*)&packet->payload().at(6), 10);

		std::shared_ptr<HMWiredPeer> peer = createPeer(packet->senderAddress(), firmwareVersion, deviceType, serialNumber, true);
		if(!peer)
		{
			GD::out.printError("Error: HomeMatic Wired Central: Could not pair device with address 0x" + BaseLib::HelperFunctions::getHexString(packet->senderAddress(), 8) + " (type: 0x" + BaseLib::HelperFunctions::getHexString(deviceType, 4) + ", firmware version: 0x" + BaseLib::HelperFunctions::getHexString(firmwareVersion, 4) + "). No matching XML file was found.");
			_peerInitMutex.unlock();
			return;
		}

		if(peerInit(peer))
		{
			PVariable deviceDescriptions(new Variable(VariableType::tArray));
			peer->restoreLinks();
			std::shared_ptr<std::vector<PVariable>> descriptions = peer->getDeviceDescriptions(nullptr, true, std::map<std::string, bool>());
			if(!descriptions)
			{
				_peerInitMutex.unlock();
				return;
			}
			for(std::vector<PVariable>::iterator j = descriptions->begin(); j != descriptions->end(); ++j)
			{
				deviceDescriptions->arrayValue->push_back(*j);
			}
            std::vector<uint64_t> newIds{ peer->getID() };
			raiseRPCNewDevices(newIds, deviceDescriptions);
		}
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	_peerInitMutex.unlock();
}

bool HMWiredCentral::peerInit(std::shared_ptr<HMWiredPeer> peer)
{
	try
	{
		peer->initializeCentralConfig();

		int32_t address = peer->getAddress();

		std::vector<uint8_t> parameterData = readEEPROM(address, 0);
		peer->binaryConfig[0].setBinaryData(parameterData);
		peer->saveParameter(peer->binaryConfig[0].databaseId, 0, parameterData);
		if(parameterData.size() != 0x10)
		{
			peer->deleteFromDatabase();
			GD::out.printError("Error: HomeMatic Wired Central: Could not pair device with address 0x" + BaseLib::HelperFunctions::getHexString(address, 8) + ". Could not read master config from EEPROM.");
			return false;
		}

		PConfigParameters configParameters = peer->getRpcDevice()->functions.at(0)->configParameters;
		for(Parameters::iterator j = configParameters->parameters.begin(); j != configParameters->parameters.end(); ++j)
		{
			if(j->second->logical->setToValueOnPairingExists)
			{
				std::vector<uint8_t> enforceValue;
				j->second->convertToPacket(j->second->logical->getSetToValueOnPairing(), Role(), enforceValue);
				peer->setConfigParameter(j->second->physical->memoryIndex, j->second->physical->size, enforceValue);
			}
		}

		parameterData = peer->binaryConfig[0].getBinaryData();
		if(!writeEEPROM(address, 0, parameterData))
		{
			GD::out.printError("Error: Could not pair device with address 0x" + BaseLib::HelperFunctions::getHexString(address, 8) + ".");
			peer->deleteFromDatabase();
			return false;
		}

		//Read all config
		std::vector<uint8_t> command({0x45, 0, 0, 0x10, 0x40}); //Request used EEPROM blocks; start address 0x0000, block size 0x10, blocks 0x40
		std::shared_ptr<HMWiredPacket> response = getResponse(command, address);
		if(!response || response->payload().empty() || response->payload().size() != 12 || response->payload().at(0) != 0x65 || response->payload().at(1) != 0 || response->payload().at(2) != 0 || response->payload().at(3) != 0x10)
		{
			GD::out.printError("Error: HomeMatic Wired Central: Could not pair device with address 0x" + BaseLib::HelperFunctions::getHexString(address, 8) + ". Could not determine EEPROM blocks to read.");
			peer->deleteFromDatabase();
			return false;
		}

		int32_t configIndex = 0;
		for(int32_t j = 0; j < 8; j++)
		{
			for(int32_t k = 0; k < 8; k++)
			{
				if(response->payload().at(j + 4) & (1 << k))
				{
					if(peer->binaryConfig.find(configIndex) == peer->binaryConfig.end())
					{
						parameterData = readEEPROM(peer->getAddress(), configIndex);
						peer->binaryConfig[configIndex].setBinaryData(parameterData);
						peer->saveParameter(peer->binaryConfig[configIndex].databaseId, configIndex, parameterData);
						if(parameterData.size() != 0x10) GD::out.printError("Error: HomeMatic Wired Central: Error reading config from device with address 0x" + BaseLib::HelperFunctions::getHexString(address, 8) + ". Size is not 16 bytes.");
					}
				}
				configIndex += 0x10;
			}
		}
		_peersMutex.lock();
		try
		{
			_peers[address] = peer;
			if(!peer->getSerialNumber().empty()) _peersBySerial[peer->getSerialNumber()] = peer;
			_peersById[peer->getID()] = peer;
			peer->setMessageCounter(_messageCounter[address]);
			_messageCounter.erase(address);
		}
		catch(const std::exception& ex)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
		}
		catch(...)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
		}
		_peersMutex.unlock();
		return true;
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	peer->deleteFromDatabase();
	return false;
}

PVariable HMWiredCentral::addLink(BaseLib::PRpcClientInfo clientInfo, std::string senderSerialNumber, int32_t senderChannelIndex, std::string receiverSerialNumber, int32_t receiverChannelIndex, std::string name, std::string description)
{
	try
	{
		if(senderSerialNumber.empty()) return Variable::createError(-2, "Given sender address is empty.");
		if(receiverSerialNumber.empty()) return Variable::createError(-2, "Given receiver address is empty.");
		std::shared_ptr<HMWiredPeer> sender = getPeer(senderSerialNumber);
		std::shared_ptr<HMWiredPeer> receiver = getPeer(receiverSerialNumber);
		if(!sender) return Variable::createError(-2, "Sender device not found.");
		if(!receiver) return Variable::createError(-2, "Receiver device not found.");
		return addLink(clientInfo, sender->getID(), senderChannelIndex, receiver->getID(), receiverChannelIndex, name, description);
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	return Variable::createError(-32500, "Unknown application error.");
}

PVariable HMWiredCentral::addLink(BaseLib::PRpcClientInfo clientInfo, uint64_t senderID, int32_t senderChannelIndex, uint64_t receiverID, int32_t receiverChannelIndex, std::string name, std::string description)
{
	try
	{
		if(senderID == 0) return Variable::createError(-2, "Given sender id is not set.");
		if(receiverID == 0) return Variable::createError(-2, "Given receiver id is not set.");
		std::shared_ptr<HMWiredPeer> sender = getPeer(senderID);
		std::shared_ptr<HMWiredPeer> receiver = getPeer(receiverID);
		if(!sender) return Variable::createError(-2, "Sender device not found.");
		if(!receiver) return Variable::createError(-2, "Receiver device not found.");
		if(senderChannelIndex < 0) senderChannelIndex = 0;
		if(receiverChannelIndex < 0) receiverChannelIndex = 0;
		std::shared_ptr<HomegearDevice> senderRpcDevice = sender->getRpcDevice();
		std::shared_ptr<HomegearDevice> receiverRpcDevice = receiver->getRpcDevice();
		Functions::iterator senderFunctionIterator = senderRpcDevice->functions.find(senderChannelIndex);
		if(senderFunctionIterator == senderRpcDevice->functions.end()) return Variable::createError(-2, "Sender channel not found.");
		Functions::iterator receiverFunctionIterator = receiverRpcDevice->functions.find(receiverChannelIndex);
		if(receiverFunctionIterator == receiverRpcDevice->functions.end()) return Variable::createError(-2, "Receiver channel not found.");
		PFunction senderFunction = senderFunctionIterator->second;
		PFunction receiverFunction = receiverFunctionIterator->second;
		if(!senderFunction || !receiverFunction || senderFunction->linkSenderFunctionTypes.size() == 0 || receiverFunction->linkReceiverFunctionTypes.size() == 0) return Variable::createError(-6, "Link not supported.");
		if(sender->getPeer(senderChannelIndex, receiver->getID(), receiverChannelIndex) || receiver->getPeer(receiverChannelIndex, sender->getID(), senderChannelIndex)) return Variable::createError(-6, "Link already exists.");
		bool validLink = false;
		for(LinkFunctionTypes::iterator i = senderFunction->linkSenderFunctionTypes.begin(); i != senderFunction->linkSenderFunctionTypes.end(); ++i)
		{
			for(LinkFunctionTypes::iterator j = receiverFunction->linkReceiverFunctionTypes.begin(); j != receiverFunction->linkReceiverFunctionTypes.end(); ++j)
			{
				if(*i == *j)
				{
					validLink = true;
					break;
				}
			}
			if(validLink) break;
		}
		if(!validLink) return Variable::createError(-6, "Link not supported.");

        bool senderLinked = false;
        bool receiverLinked = false;

		auto senderPeers = sender->getLinkPeers(clientInfo, senderChannelIndex, true);
        for(auto& linkedPeer : *senderPeers->arrayValue)
        {
            if((unsigned)linkedPeer->arrayValue->at(0)->integerValue64 == receiver->getID() && linkedPeer->arrayValue->at(1)->integerValue == receiverChannelIndex)
            {
                senderLinked = true;
                break;
            }
        }

        auto receiverPeers = receiver->getLinkPeers(clientInfo, receiverChannelIndex, true);
        for(auto& linkedPeer : *receiverPeers->arrayValue)
        {
            if((unsigned)linkedPeer->arrayValue->at(0)->integerValue64 == sender->getID() && linkedPeer->arrayValue->at(1)->integerValue == senderChannelIndex)
            {
                receiverLinked = true;
                break;
            }
        }

        std::shared_ptr<BaseLib::Systems::BasicPeer> senderPeer(new BaseLib::Systems::BasicPeer());
        if(!senderLinked)
        {
            senderPeer->isSender = true;
            senderPeer->id = sender->getID();
            senderPeer->address = sender->getAddress();
            senderPeer->channel = senderChannelIndex;
            senderPeer->physicalIndexOffset = senderFunction->physicalChannelIndexOffset;
            senderPeer->serialNumber = sender->getSerialNumber();
            senderPeer->isSender = true;
            senderPeer->linkDescription = description;
            senderPeer->linkName = name;
            senderPeer->configEEPROMAddress = receiver->getFreeEEPROMAddress(receiverChannelIndex, false);
            if(senderPeer->configEEPROMAddress == -1) return Variable::createError(-32500, "Can't get free eeprom address to store config.");
        }

        std::shared_ptr<BaseLib::Systems::BasicPeer> receiverPeer(new BaseLib::Systems::BasicPeer());
        if(!receiverLinked)
        {
            receiverPeer->id = receiver->getID();
            receiverPeer->address = receiver->getAddress();
            receiverPeer->channel = receiverChannelIndex;
            receiverPeer->physicalIndexOffset = receiverFunction->physicalChannelIndexOffset;
            receiverPeer->serialNumber = receiver->getSerialNumber();
            receiverPeer->linkDescription = description;
            receiverPeer->linkName = name;
            receiverPeer->configEEPROMAddress = sender->getFreeEEPROMAddress(senderChannelIndex, true);
            if(receiverPeer->configEEPROMAddress == -1) return Variable::createError(-32500, "Can't get free eeprom address to store config.");
        }

        if(!senderLinked)
        {
            sender->addPeer(senderChannelIndex, receiverPeer);
            sender->initializeLinkConfig(senderChannelIndex, receiverPeer);
            raiseRPCUpdateDevice(sender->getID(), senderChannelIndex, sender->getSerialNumber() + ":" + std::to_string(senderChannelIndex), 1);
        }

        if(!receiverLinked)
        {
            receiver->addPeer(receiverChannelIndex, senderPeer);
            receiver->initializeLinkConfig(receiverChannelIndex, senderPeer);
            raiseRPCUpdateDevice(receiver->getID(), receiverChannelIndex, receiver->getSerialNumber() + ":" + std::to_string(receiverChannelIndex), 1);
        }

		return std::make_shared<Variable>(VariableType::tVoid);
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	return Variable::createError(-32500, "Unknown application error.");
}

PVariable HMWiredCentral::deleteDevice(BaseLib::PRpcClientInfo clientInfo, std::string serialNumber, int32_t flags)
{
	try
	{
		if(serialNumber.empty()) return Variable::createError(-2, "Unknown device.");

		uint64_t peerId = 0;

		{
			std::shared_ptr<HMWiredPeer> peer = getPeer(serialNumber);
			if(!peer) return PVariable(new Variable(VariableType::tVoid));
			peerId = peer->getID();
		}

		return deleteDevice(clientInfo, peerId, flags);
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    return Variable::createError(-32500, "Unknown application error.");
}

PVariable HMWiredCentral::deleteDevice(BaseLib::PRpcClientInfo clientInfo, uint64_t peerID, int32_t flags)
{
	try
	{
		if(peerID == 0) return Variable::createError(-2, "Unknown device.");
		std::shared_ptr<HMWiredPeer> peer = getPeer(peerID);
		if(!peer) return PVariable(new Variable(VariableType::tVoid));
		uint64_t id = peer->getID();

		//Reset
		if(flags & 0x01) peer->reset();
		peer.reset();
		deletePeer(id);

		if(peerExists(id)) return Variable::createError(-1, "Error deleting peer. See log for more details.");

		return PVariable(new Variable(VariableType::tVoid));
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    return Variable::createError(-32500, "Unknown application error.");
}

/*PVariable HMWiredCentral::getDeviceDescriptionCentral()
{
	try
	{
		PVariable description(new Variable(VariableType::tStruct));

		description->structValue->insert(StructElement("ID", PVariable(new Variable(_deviceId))));
		description->structValue->insert(StructElement("ADDRESS", PVariable(new Variable(_serialNumber))));

		PVariable variable = PVariable(new Variable(VariableType::tArray));
		description->structValue->insert(StructElement("CHILDREN", variable));

		description->structValue->insert(StructElement("FIRMWARE", PVariable(new Variable(std::string(VERSION)))));

		int32_t uiFlags = (int32_t)Rpc::Device::UIFlags::dontdelete | (int32_t)Rpc::Device::UIFlags::visible;
		description->structValue->insert(StructElement("FLAGS", PVariable(new Variable(uiFlags))));

		description->structValue->insert(StructElement("INTERFACE", PVariable(new Variable(_serialNumber))));

		variable = PVariable(new Variable(VariableType::tArray));
		description->structValue->insert(StructElement("PARAMSETS", variable));
		variable->arrayValue->push_back(PVariable(new Variable(std::string("MASTER")))); //Always MASTER

		description->structValue->insert(StructElement("PARENT", PVariable(new Variable(std::string("")))));

		description->structValue->insert(StructElement("PHYSICAL_ADDRESS", PVariable(new Variable(std::to_string(_address)))));
		description->structValue->insert(StructElement("RF_ADDRESS", PVariable(new Variable(std::to_string(_address)))));

		description->structValue->insert(StructElement("ROAMING", PVariable(new Variable(0))));

		description->structValue->insert(StructElement("TYPE", PVariable(new Variable(std::string("Homegear HomeMatic Wired Central")))));

		description->structValue->insert(StructElement("VERSION", PVariable(new Variable((int32_t)10))));

		return description;
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    return Variable::createError(-32500, "Unknown application error.");
}*/

PVariable HMWiredCentral::removeLink(BaseLib::PRpcClientInfo clientInfo, std::string senderSerialNumber, int32_t senderChannelIndex, std::string receiverSerialNumber, int32_t receiverChannelIndex)
{
	try
	{
		if(senderSerialNumber.empty()) return Variable::createError(-2, "Given sender address is empty.");
		if(receiverSerialNumber.empty()) return Variable::createError(-2, "Given receiver address is empty.");
		std::shared_ptr<HMWiredPeer> sender = getPeer(senderSerialNumber);
		std::shared_ptr<HMWiredPeer> receiver = getPeer(receiverSerialNumber);
		if(!sender) return Variable::createError(-2, "Sender device not found.");
		if(!receiver) return Variable::createError(-2, "Receiver device not found.");
		return removeLink(clientInfo, sender->getID(), senderChannelIndex, receiver->getID(), receiverChannelIndex);
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	return Variable::createError(-32500, "Unknown application error.");
}

PVariable HMWiredCentral::removeLink(BaseLib::PRpcClientInfo clientInfo, uint64_t senderID, int32_t senderChannelIndex, uint64_t receiverID, int32_t receiverChannelIndex)
{
	try
	{
		if(senderID == 0) return Variable::createError(-2, "Sender id is not set.");
		if(receiverID == 0) return Variable::createError(-2, "Receiver id is not set.");
		std::shared_ptr<HMWiredPeer> sender = getPeer(senderID);
		std::shared_ptr<HMWiredPeer> receiver = getPeer(receiverID);
		if(!sender) return Variable::createError(-2, "Sender device not found.");
		if(!receiver) return Variable::createError(-2, "Receiver device not found.");
		if(senderChannelIndex < 0) senderChannelIndex = 0;
		if(receiverChannelIndex < 0) receiverChannelIndex = 0;
		std::shared_ptr<HomegearDevice> senderRpcDevice = sender->getRpcDevice();
		std::shared_ptr<HomegearDevice> receiverRpcDevice = receiver->getRpcDevice();
		if(senderRpcDevice->functions.find(senderChannelIndex) == senderRpcDevice->functions.end()) return Variable::createError(-2, "Sender channel not found.");
		if(receiverRpcDevice->functions.find(receiverChannelIndex) == receiverRpcDevice->functions.end()) return Variable::createError(-2, "Receiver channel not found.");
		if(!sender->getPeer(senderChannelIndex, receiver->getID()) && !receiver->getPeer(receiverChannelIndex, sender->getID())) return Variable::createError(-6, "Devices are not paired to each other.");

		sender->removePeer(senderChannelIndex, receiver->getID(), receiverChannelIndex);
		receiver->removePeer(receiverChannelIndex, sender->getID(), senderChannelIndex);

		raiseRPCUpdateDevice(sender->getID(), senderChannelIndex, sender->getSerialNumber() + ":" + std::to_string(senderChannelIndex), 1);
		raiseRPCUpdateDevice(receiver->getID(), receiverChannelIndex, receiver->getSerialNumber() + ":" + std::to_string(receiverChannelIndex), 1);

		return PVariable(new Variable(VariableType::tVoid));
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	return Variable::createError(-32500, "Unknown application error.");
}

PVariable HMWiredCentral::searchDevices(BaseLib::PRpcClientInfo clientInfo, const std::string& interfaceId)
{
	try
	{
		lockBus();
		_pairing = true;
		std::vector<int32_t> foundDevices;
		GD::physicalInterface->search(foundDevices);
		unlockBus();
		GD::out.printInfo("Info: Search completed. Found " + std::to_string(foundDevices.size()) + " devices.");
		std::vector<std::shared_ptr<HMWiredPeer>> newPeers;
		_peerInitMutex.lock();
		try
		{
			for(std::vector<int32_t>::iterator i = foundDevices.begin(); i != foundDevices.end(); ++i)
			{
				if(getPeer(*i)) continue;

				//Get device type:
				std::shared_ptr<HMWiredPacket> response = getResponse(0x68, *i, true);
				if(!response || response->payload().size() != 2)
				{
					GD::out.printError("Error: HomeMatic Wired Central: Could not pair device with address 0x" + BaseLib::HelperFunctions::getHexString(*i, 8) + ". Device type request failed.");
					continue;
				}
				uint32_t deviceType = (response->payload().at(0) << 8) + response->payload().at(1);

				//Get firmware version:
				response = getResponse(0x76, *i);
				if(!response || response->payload().size() != 2)
				{
					GD::out.printError("Error: HomeMatic Wired Central: Could not pair device with address 0x" + BaseLib::HelperFunctions::getHexString(*i, 8) + ". Firmware version request failed.");
					continue;
				}
				int32_t firmwareVersion = (response->payload().at(0) << 8) + response->payload().at(1);

				//Get serial number:
				response = getResponse(0x6E, *i);
				if(!response || response->payload().empty())
				{
					GD::out.printError("Error: HomeMatic Wired Central: Could not pair device with address 0x" + BaseLib::HelperFunctions::getHexString(*i, 8) + ". Serial number request failed.");
					continue;
				}
				std::string serialNumber((char*)&response->payload().at(0), response->payload().size());

				std::shared_ptr<HMWiredPeer> peer = createPeer(*i, firmwareVersion, deviceType, serialNumber, true);
				if(!peer)
				{
					GD::out.printError("Error: HomeMatic Wired Central: Could not pair device with address 0x" + BaseLib::HelperFunctions::getHexString(*i, 8) + " (type: 0x" + BaseLib::HelperFunctions::getHexString(deviceType, 4) + ", firmware version: 0x" + BaseLib::HelperFunctions::getHexString(firmwareVersion, 4) + "). No matching XML file was found.");
					continue;
				}

				if(peerInit(peer)) newPeers.push_back(peer);;
			}

			if(newPeers.size() > 0)
			{
				std::vector<uint64_t> newIds;
				newIds.reserve(newPeers.size());
				PVariable deviceDescriptions(new Variable(VariableType::tArray));
				for(std::vector<std::shared_ptr<HMWiredPeer>>::iterator i = newPeers.begin(); i != newPeers.end(); ++i)
				{
					(*i)->restoreLinks();
					std::shared_ptr<std::vector<PVariable>> descriptions = (*i)->getDeviceDescriptions(clientInfo, true, std::map<std::string, bool>());
					if(!descriptions) continue;
					newIds.push_back((*i)->getID());
					for(std::vector<PVariable>::iterator j = descriptions->begin(); j != descriptions->end(); ++j)
					{
						deviceDescriptions->arrayValue->push_back(*j);
					}
				}
				raiseRPCNewDevices(newIds, deviceDescriptions);
			}
		}
		catch(const std::exception& ex)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
		}
		catch(...)
		{
			GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
		}
		_peerInitMutex.unlock();
		_pairing = false;
		return PVariable(new Variable((uint32_t)newPeers.size()));
	}
	catch(const std::exception& ex)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
	}
	catch(...)
	{
		GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
	}
	_pairing = false;
	unlockBus();
	return Variable::createError(-32500, "Unknown application error.");
}

PVariable HMWiredCentral::updateFirmware(BaseLib::PRpcClientInfo clientInfo, std::vector<uint64_t> ids, bool manual)
{
	try
	{
		if(_updateMode || _bl->deviceUpdateInfo.currentDevice > 0) return Variable::createError(-32500, "Central is already already updating a device. Please wait until the current update is finished.");
		_updateFirmwareThreadMutex.lock();
		if(_disposing)
		{
			_updateFirmwareThreadMutex.unlock();
			return Variable::createError(-32500, "Central is disposing.");
		}
		_bl->threadManager.join(_updateFirmwareThread);
		_bl->threadManager.start(_updateFirmwareThread, false, &HMWiredCentral::updateFirmwares, this, ids);
		_updateFirmwareThreadMutex.unlock();
		return PVariable(new Variable(true));
	}
	catch(const std::exception& ex)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__, ex.what());
    }
    catch(...)
    {
        GD::out.printEx(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    return Variable::createError(-32500, "Unknown application error.");
}
} /* namespace HMWired */
