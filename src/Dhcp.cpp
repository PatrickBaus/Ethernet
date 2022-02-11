// DHCP Library v0.3 - April 25, 2009
// Author: Jordan Terrell - blog.jordanterrell.com

#include <Arduino.h>
#include "Ethernet.h"
#include "Dhcp.h"
#include "utility/w5100.h"

//return:0 on error, non-zero otherwise
uint8_t DhcpClass::initDHCPRequest(uint8_t *mac, unsigned long responseTimeout)
{
	_dhcpLeaseTime=0;
	_dhcpT1=0;
	_dhcpT2=0;
	_responseTimeout = responseTimeout;

	// zero out _dhcpMacAddr
	memset(_dhcpMacAddr, 0, 6);
	reset_DHCP_lease();

	memcpy((void*)_dhcpMacAddr, (void*)mac, 6);
	_dhcp_state = STATE_DHCP_START;

	return prepareDHCPSocket();
}

uint8_t DhcpClass::prepareDHCPSocket() {
	// Pick an initial transaction ID
	_dhcpTransactionId = random(1UL, 2000UL);
	_dhcpInitialTransactionId = _dhcpTransactionId;

	_dhcpUdpSocket.stop();
	if (_dhcpUdpSocket.begin(DHCP_CLIENT_PORT) == 0) {
		// Couldn't get a socket
		return 0;
	}
	_startOfLeaseRequest = millis();
	presend_DHCP();
	return 2;
}

void DhcpClass::reset_DHCP_lease()
{
	// zero out _dhcpSubnetMask, _dhcpGatewayIp, _dhcpLocalIp, _dhcpDhcpServerIp, _dhcpDnsServerIp
	memset(_dhcpLocalIp, 0, 20);
}

// Return 0 on error, 1 when we have a lease/success, 2 when waiting
int DhcpClass::processDHCPRequestAsync()
{
	if (_dhcp_state == STATE_DHCP_LEASED) {
		return 1;   // Success (1)
	}

	int result;
	uint32_t respId;
	uint8_t messageType;
	static unsigned long timeoutCounter;
	static unsigned long sleepCounter;

	switch (_dhcp_state) {
		case STATE_DHCP_START:
			_dhcpTransactionId++;
			send_DHCP_MESSAGE(DHCP_DISCOVER, ((millis() - _startOfLeaseRequest) / 1000));
			_dhcp_state = STATE_WAIT_FOR_DHCP_OFFER;
			result = 2;	 // Waiting (2)
			timeoutCounter = sleepCounter = millis();
			break;
		case STATE_WAIT_FOR_DHCP_OFFER:
			if (millis() - sleepCounter < 50) {
				result = 2;	 // Waiting (2)
			} else {
				sleepCounter = millis();
				switch (waitForDHCPResponse(timeoutCounter)) {
					case 1:	 // Success (1)
						_dhcp_state = STATE_DHCP_DISCOVER;
					case 2:	 // Waiting (2)
						result = 2;
						break;
					case 255:   // Timeout (255)
						_dhcp_state = STATE_DHCP_START;
						break;
					default:
						result = 0;
				}
			}
			break;
		case STATE_DHCP_DISCOVER:
			messageType = parseDHCPResponse(_responseTimeout, respId);
			if (messageType == DHCP_OFFER) {
				// We'll use the transaction ID that the offer came with,
				// rather than the one we were up to
				_dhcpTransactionId = respId;
				send_DHCP_MESSAGE(DHCP_REQUEST, ((millis() - _startOfLeaseRequest) / 1000));
				timeoutCounter = sleepCounter = millis();
				_dhcp_state = STATE_WAIT_FOR_DHCP_ACK;
			}
			else {
				_dhcp_state = STATE_WAIT_FOR_DHCP_OFFER;
			}
			result = 2; // Waiting (2)
			timeoutCounter = sleepCounter = millis();
			break;
		case STATE_WAIT_FOR_DHCP_ACK:
			if (millis() - sleepCounter < 50) {
				result = 2;	 // Waiting (2)
			} else {
				sleepCounter = millis();
				switch (waitForDHCPResponse(timeoutCounter)) {
					case 1:	 // Success (1)
						_dhcp_state = STATE_DHCP_REQUEST;
					case 2:	 // Waiting (2)
						result = 2;
						break;
					case 255:   // Timeout (255)
						_dhcp_state = STATE_DHCP_START;
						break;
					default:
						result = 0;
				}
			}
			break;
		case STATE_DHCP_REQUEST:
			messageType = parseDHCPResponse(_responseTimeout, respId);
			if (messageType == DHCP_ACK) {
				//use default lease time if we didn't get it
				if (_dhcpLeaseTime == 0) {
					_dhcpLeaseTime = DEFAULT_LEASE * 1000;
				}
				// Calculate T1 & T2 if we didn't get it
				if (_dhcpT1 == 0) {
					// T1 should be 50% of _dhcpLeaseTime
					_dhcpT1 = _dhcpLeaseTime >> 1;
				}
				if (_dhcpT2 == 0) {
					// T2 should be 87.5% (7/8ths) of _dhcpLeaseTime
					_dhcpT2 = _dhcpLeaseTime - (_dhcpLeaseTime >> 3);
				}
				_renewInMilliSec = _dhcpT1;
				_rebindInMilliSec = _dhcpT2;
				_lastCheckLeaseMillis = millis();
				result = 1;
				_dhcp_state = STATE_DHCP_LEASED;
				_dhcpTransactionId++;
			} else if (messageType == DHCP_NAK) {
				result = 2;
				_dhcp_state = STATE_DHCP_START;
			} else {
				_dhcp_state = STATE_WAIT_FOR_DHCP_ACK;
				result = 2;
				timeoutCounter = sleepCounter = millis();
			}
			break;
		case STATE_DHCP_REREQUEST:
			_dhcpTransactionId++;
			send_DHCP_MESSAGE(DHCP_REQUEST, ((millis() - _startOfLeaseRequest)/1000));
			_dhcp_state = STATE_DHCP_REQUEST;
			result = 2;
			break;
		default:
			result = 0; // unknown error
	}

	return result;
}

void DhcpClass::presend_DHCP()
{
}

void DhcpClass::send_DHCP_MESSAGE(uint8_t messageType, uint16_t secondsElapsed)
{
	uint8_t buffer[32];
	memset(buffer, 0, 32);
	IPAddress dest_addr(255, 255, 255, 255); // Broadcast address

	if (_dhcpUdpSocket.beginPacket(dest_addr, DHCP_SERVER_PORT) == -1) {
		//Serial.printf("DHCP transmit error\n");
		// FIXME Need to return errors
		return;
	}

	buffer[0] = DHCP_BOOTREQUEST;   // op
	buffer[1] = DHCP_HTYPE10MB;     // htype
	buffer[2] = DHCP_HLENETHERNET;  // hlen
	buffer[3] = DHCP_HOPS;          // hops

	// xid
	unsigned long xid = htonl(_dhcpTransactionId);
	memcpy(buffer + 4, &(xid), 4);

	// 8, 9 - seconds elapsed
	buffer[8] = ((secondsElapsed & 0xff00) >> 8);
	buffer[9] = (secondsElapsed & 0x00ff);

	// flags
	unsigned short flags = htons(DHCP_FLAGSBROADCAST);
	memcpy(buffer + 10, &(flags), 2);

	// ciaddr: already zeroed
	// yiaddr: already zeroed
	// siaddr: already zeroed
	// giaddr: already zeroed

	//put data in W5100 transmit buffer
	_dhcpUdpSocket.write(buffer, 28);

	memset(buffer, 0, 32); // clear local buffer

	memcpy(buffer, _dhcpMacAddr, 6); // chaddr

	//put data in W5100 transmit buffer
	_dhcpUdpSocket.write(buffer, 16);

	memset(buffer, 0, 32); // clear local buffer

	// leave zeroed out for sname && file
	// put in W5100 transmit buffer x 6 (192 bytes)

	for(int i = 0; i < 6; i++) {
		_dhcpUdpSocket.write(buffer, 32);
	}

	// OPT - Magic Cookie
	buffer[0] = (uint8_t)((MAGIC_COOKIE >> 24)& 0xFF);
	buffer[1] = (uint8_t)((MAGIC_COOKIE >> 16)& 0xFF);
	buffer[2] = (uint8_t)((MAGIC_COOKIE >> 8)& 0xFF);
	buffer[3] = (uint8_t)(MAGIC_COOKIE& 0xFF);

	// OPT - message type
	buffer[4] = dhcpMessageType;
	buffer[5] = 0x01;
	buffer[6] = messageType; //DHCP_REQUEST;

	// OPT - client identifier
	buffer[7] = dhcpClientIdentifier;
	buffer[8] = 0x07;
	buffer[9] = 0x01;
	memcpy(buffer + 10, _dhcpMacAddr, 6);

	// OPT - host name
	buffer[16] = hostName;
	buffer[17] = strlen(HOST_NAME) + 6; // length of hostname + last 3 bytes of mac address
	strcpy((char*)&(buffer[18]), HOST_NAME);

	printByte((char*)&(buffer[24]), _dhcpMacAddr[3]);
	printByte((char*)&(buffer[26]), _dhcpMacAddr[4]);
	printByte((char*)&(buffer[28]), _dhcpMacAddr[5]);

	//put data in W5100 transmit buffer
	_dhcpUdpSocket.write(buffer, 30);

	if (messageType == DHCP_REQUEST) {
		buffer[0] = dhcpRequestedIPaddr;
		buffer[1] = 0x04;
		buffer[2] = _dhcpLocalIp[0];
		buffer[3] = _dhcpLocalIp[1];
		buffer[4] = _dhcpLocalIp[2];
		buffer[5] = _dhcpLocalIp[3];

		buffer[6] = dhcpServerIdentifier;
		buffer[7] = 0x04;
		buffer[8] = _dhcpDhcpServerIp[0];
		buffer[9] = _dhcpDhcpServerIp[1];
		buffer[10] = _dhcpDhcpServerIp[2];
		buffer[11] = _dhcpDhcpServerIp[3];

		//put data in W5100 transmit buffer
		_dhcpUdpSocket.write(buffer, 12);
	}

	buffer[0] = dhcpParamRequest;
	buffer[1] = 0x06;
	buffer[2] = subnetMask;
	buffer[3] = routersOnSubnet;
	buffer[4] = dns;
	buffer[5] = domainName;
	buffer[6] = dhcpT1value;
	buffer[7] = dhcpT2value;
	buffer[8] = endOption;

	//put data in W5100 transmit buffer
	_dhcpUdpSocket.write(buffer, 9);

	_dhcpUdpSocket.endPacket();
}

uint8_t DhcpClass::waitForDHCPResponse(unsigned long startTime) {
	if (_dhcpUdpSocket.parsePacket() <= 0) {
		if ((millis() - startTime) > _responseTimeout) {
			return 255;  // Timeout (255)
		}
		return 2; // Waiting (2)
	}
	return 1;   // Success (1)
}

// returns 255 on timeout
uint8_t DhcpClass::parseDHCPResponse(unsigned long responseTimeout, uint32_t& transactionId)
{
	uint8_t type = 0;
	uint8_t opt_len = 0;

	// start reading in the packet
	RIP_MSG_FIXED fixedMsg;
	_dhcpUdpSocket.read((uint8_t*)&fixedMsg, sizeof(RIP_MSG_FIXED));

	if (fixedMsg.op == DHCP_BOOTREPLY && _dhcpUdpSocket.remotePort() == DHCP_SERVER_PORT) {
		transactionId = ntohl(fixedMsg.xid);
		if (memcmp(fixedMsg.chaddr, _dhcpMacAddr, 6) != 0 ||
		  (transactionId < _dhcpInitialTransactionId) ||
		  (transactionId > _dhcpTransactionId)) {
			// Need to read the rest of the packet here regardless
			_dhcpUdpSocket.flush(); // FIXME
			return 0;
		}

		memcpy(_dhcpLocalIp, fixedMsg.yiaddr, 4);

		// Skip to the option part
		_dhcpUdpSocket.read((uint8_t *)NULL, 240 - (int)sizeof(RIP_MSG_FIXED));

		while (_dhcpUdpSocket.available() > 0) {
			switch (_dhcpUdpSocket.read()) {
			case endOption :
				break;

			case padOption :
				break;

			case dhcpMessageType :
				opt_len = _dhcpUdpSocket.read();
				type = _dhcpUdpSocket.read();
				break;

			case subnetMask :
				opt_len = _dhcpUdpSocket.read();
				_dhcpUdpSocket.read(_dhcpSubnetMask, 4);
				break;

			case routersOnSubnet :
				opt_len = _dhcpUdpSocket.read();
				_dhcpUdpSocket.read(_dhcpGatewayIp, 4);
				_dhcpUdpSocket.read((uint8_t *)NULL, opt_len - 4);
				break;

			case dns :
				opt_len = _dhcpUdpSocket.read();
				_dhcpUdpSocket.read(_dhcpDnsServerIp, 4);
				_dhcpUdpSocket.read((uint8_t *)NULL, opt_len - 4);
				break;

			case dhcpServerIdentifier :
				opt_len = _dhcpUdpSocket.read();
				if ( IPAddress(_dhcpDhcpServerIp) == IPAddress((uint32_t)0) ||
				  IPAddress(_dhcpDhcpServerIp) == _dhcpUdpSocket.remoteIP() ) {
					_dhcpUdpSocket.read(_dhcpDhcpServerIp, sizeof(_dhcpDhcpServerIp));
				} else {
					// Skip over the rest of this option
					_dhcpUdpSocket.read((uint8_t *)NULL, opt_len);
				}
				break;

			case dhcpT1value :
				opt_len = _dhcpUdpSocket.read();
				_dhcpUdpSocket.read((uint8_t*)&_dhcpT1, sizeof(_dhcpT1));
				_dhcpT1 = ntohl(_dhcpT1) * 1000;	// in ms
				break;

			case dhcpT2value :
				opt_len = _dhcpUdpSocket.read();
				_dhcpUdpSocket.read((uint8_t*)&_dhcpT2, sizeof(_dhcpT2));
				_dhcpT2 = ntohl(_dhcpT2) * 1000;	// in ms
				break;

			case dhcpIPaddrLeaseTime :
				opt_len = _dhcpUdpSocket.read();
				_dhcpUdpSocket.read((uint8_t*)&_dhcpLeaseTime, sizeof(_dhcpLeaseTime));
				_dhcpLeaseTime = ntohl(_dhcpLeaseTime);
				_renewInMilliSec = _dhcpLeaseTime * 1000;
				break;

			default :
				opt_len = _dhcpUdpSocket.read();
				// Skip over the rest of this option
				_dhcpUdpSocket.read((uint8_t *)NULL, opt_len);
				break;
			}
		}
	}

	// Need to skip to end of the packet regardless here
	_dhcpUdpSocket.flush(); // FIXME

	return type;
}

/*
	returns:
	0/DHCP_CHECK_NONE: nothing happened
	1/DHCP_CHECK_RENEW_FAIL: renew failed
	3/DHCP_CHECK_RENEW_WAIT: waiting for renewal
	4/DHCP_CHECK_REBIND_FAIL: rebind fail
	6/DHCP_CHECK_REBIND_OK: waiting for rebind
*/
int DhcpClass::checkLease()
{
	int rc = DHCP_CHECK_NONE;

	unsigned long now = millis();
	unsigned long elapsed = now - _lastCheckLeaseMillis;

	// if more then one sec passed, reduce the counters accordingly
	if (elapsed >= 1000) {
		// set the new timestamps
		_lastCheckLeaseMillis = now;

		// decrease the counters by elapsed seconds
		// we assume that the cycle time (elapsed) is fairly constant
		// if the remainder is less than cycle time * 2
		// do it early instead of late
		if (_renewInMilliSec < elapsed * 2) {
			_renewInMilliSec = 0;
		} else {
			_renewInMilliSec -= elapsed;
		}
		if (_rebindInMilliSec < elapsed * 2) {
			_rebindInMilliSec = 0;
		} else {
			_rebindInMilliSec -= elapsed;
		}
		if (_dhcpLeaseTime < elapsed) {
			_dhcpLeaseTime = 0;
		} else {
			_dhcpLeaseTime -= elapsed;
		}
	}

	if (_dhcpLeaseTime == 0 and (_dhcp_state == STATE_DHCP_LEASED or _dhcp_state==STATE_WAIT_FOR_DHCP_OFFER)) {
		reset_DHCP_lease();
	}

	// if we have a lease or is renewing but should bind, do it
	if (_rebindInMilliSec == 0 && _dhcp_state == STATE_DHCP_LEASED) {
		// this should basically restart completely
		_dhcp_state = STATE_DHCP_START;
		//reset_DHCP_lease();
		rc = DHCP_CHECK_REBIND_FAIL + prepareDHCPSocket();  // Returns 0 on failure and 2 when in progress
	}
	// if we have a lease but should renew, do it
	else if (_renewInMilliSec == 0 && _dhcp_state == STATE_DHCP_LEASED) {
		_dhcp_state = STATE_DHCP_REREQUEST;
		rc = DHCP_CHECK_RENEW_FAIL + prepareDHCPSocket(); // Returns 0 on failure and 2 when in progress
	}
	return rc;
}

IPAddress DhcpClass::getLocalIp()
{
	return IPAddress(_dhcpLocalIp);
}

IPAddress DhcpClass::getSubnetMask()
{
	return IPAddress(_dhcpSubnetMask);
}

IPAddress DhcpClass::getGatewayIp()
{
	return IPAddress(_dhcpGatewayIp);
}

IPAddress DhcpClass::getDhcpServerIp()
{
	return IPAddress(_dhcpDhcpServerIp);
}

IPAddress DhcpClass::getDnsServerIp()
{
	return IPAddress(_dhcpDnsServerIp);
}

void DhcpClass::printByte(char * buf, uint8_t n )
{
	char *str = &buf[1];
	buf[0]='0';
	do {
		unsigned long m = n;
		n /= 16;
		char c = m - 16 * n;
		*str-- = c < 10 ? c + '0' : c + 'A' - 10;
	} while(n);
}
