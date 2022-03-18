/*********************************************************************************
 Copyright 2017 GlobalPlatform, Inc.

 Licensed under the GlobalPlatform/Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 https://github.com/GlobalPlatform/SE-test-IP-connector/blob/master/Charter%20and%20Rules%20for%20the%20SE%20IP%20connector.docx

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 *********************************************************************************/

#define _WIN32_WINNT 0x601
#define WIN32_LEAN_AND_MEAN

#include "server/server_tcp_socket.hpp"
#include "plog/include/plog/Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace server {

inline const char* getWsaErrorDescription(int wsaError)
{
	static char _wsaDescription[1024];
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, wsaError, 0, _wsaDescription, 1024, NULL);
	return _wsaDescription;
}

bool ServerTCPSocket::startServer(const char* ip, const char* port) {
	int retval = 0;

	// initializes Winsock
	retval = WSAStartup(MAKEWORD(2, 2), &wsaData_);
	if (retval != 0) {
		LOG_DEBUG << "Failed to call WSAStartup()";
		return false;
	}

	ZeroMemory(&hints_, sizeof(hints_));
	hints_.ai_family = AF_INET;
	hints_.ai_socktype = SOCK_STREAM;
	hints_.ai_protocol = IPPROTO_TCP;
	hints_.ai_flags = AI_PASSIVE;

	// Resolve the server address and port
	retval = getaddrinfo(ip, port, &hints_, &result_);
	if (retval != 0) {
		WSACleanup();
		LOG_DEBUG << "Failed to call getaddrinfo() " << "[ip:" << ip << "][port:" << port << "]";
		return false;
	}

	// Create a SOCKET for connecting to server
	server_socket_ = socket(result_->ai_family, result_->ai_socktype, result_->ai_protocol);
	if (server_socket_ == INVALID_SOCKET) {
		LOG_DEBUG << "Failed to call socket() " << "[ip:" << ip << "][port:" << port << "][WSAError:" << WSAGetLastError() << "]";
		freeaddrinfo(result_);
		WSACleanup();
		return false;
	}

	// Setup the TCP listening socket
	retval = bind(server_socket_, result_->ai_addr, (int) result_->ai_addrlen);
	if (retval == SOCKET_ERROR) {
		LOG_DEBUG << "Failed to call bind() " << "[ip:" << ip << "][port:" << port << "][WSAError:" << WSAGetLastError() << "]";
		closeServer();
		return false;
	}
	freeaddrinfo(result_);

	retval = listen(server_socket_, SOMAXCONN);
	if (retval == SOCKET_ERROR) {
		LOG_DEBUG << "Failed to call listen() " << "[ip:" << ip << "][port:" << port << "][WSAError:" << WSAGetLastError() << "]";
		closeServer();
		return false;
	}
	return true;
}

bool ServerTCPSocket::acceptConnection(SOCKET* client_socket, int default_timeout) {
	LOG_INFO << "acceptConnection started";

	*client_socket = accept(server_socket_, NULL, NULL);
	if (*client_socket == INVALID_SOCKET) {
		LOG_DEBUG << "Failed to call accept() " << "[listen_socket:" << server_socket_ << "][WSAError:" << WSAGetLastError() << "]";
		return false;
	}

	if (setsockopt(*client_socket, SOL_SOCKET, SO_RCVTIMEO, (char*) &default_timeout, sizeof(default_timeout)) < 0) {
		LOG_DEBUG << "Failed to call setsockopt() " << "[listen_socket:" << server_socket_ << "][WSAError:" << WSAGetLastError() << "]";
		return false;
	}

	LOG_INFO << "acceptConnection succeeded";
	return true;
}

bool ServerTCPSocket::sendData(SOCKET client_socket, const char* data, int size) {
	int retval = 0;
	do {
		retval = send(client_socket, data, size, 0);
		if (retval == SOCKET_ERROR) {
			LOG_DEBUG << "Failed to send data to client -  " << "[socket:" << client_socket << "][buffer:" << data << "][size:" << size << "][flags:" << NULL << "]";
			return false;
		}
		data += retval;
		size -= retval;
	} while (size > 0);
	return true;
}

bool ServerTCPSocket::sendPacket(SOCKET client_socket, const char* packet) {
	int packet_size = strlen(packet);
	int net_packet_size = htonl(packet_size); // deals with endianness

	// send packet's content size
	int retval = send(client_socket, (char*) &net_packet_size, sizeof(int), 0);
	if (retval == SOCKET_ERROR || retval == 0) {
		LOG_DEBUG << "Failed to send data size to client -  " << "[socket:" << client_socket << "][buffer:" << net_packet_size << "][size:" << sizeof(int) << "][flags:" << NULL << "]";
		return false;
	}

	// send packet's content
	if (!sendData(client_socket, packet, packet_size)) return false;

	return true;
}

int ServerTCPSocket::receivePacket(SOCKET client_socket, char* packet) {
	int received_size = 0;
	int net_received_size = 0;

	// retrieve packet size
	int retval = recv(client_socket, (char*) &net_received_size, sizeof(int), MSG_WAITALL);
	if (retval == SOCKET_ERROR || retval == 0) {
		int wsaError = WSAGetLastError();
		LOG_DEBUG << "Failed to receive data size from client -  " << "[socket:" << client_socket << "][buffer:" << received_size << "][size:" << sizeof(int) << "][flags:" << NULL << "][WSAError:"<< wsaError << " " << getWsaErrorDescription(wsaError) << "]";
		return wsaError == WSAEWOULDBLOCK ? RES_SOCKET_WARNING : RES_SOCKET_ERROR;
	}
	received_size = ntohl(net_received_size); // deal with endianness

	// retrieve packet
	retval = recv(client_socket, packet, received_size, MSG_WAITALL); // keep receiving until received_size bytes are received
	if (retval == SOCKET_ERROR || retval == 0) {
		LOG_DEBUG << "Failed to receive data size from client -  " << "[socket:" << client_socket << "][buffer:" << received_size << "][size:" << sizeof(int) << "][flags:" << NULL << "]";
		return RES_SOCKET_ERROR;
	}
	packet[retval] = '\0';

	return RES_SOCKET_OK;
}

void ServerTCPSocket::closeServer() {
	if (server_socket_ != INVALID_SOCKET) {
		closesocket(server_socket_);
		server_socket_ = INVALID_SOCKET;
	}
	WSACleanup();
}

} /* namespace server */
