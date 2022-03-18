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

#include "server/client_data.hpp"
#include "server/server_engine.hpp"
#include "config/config_wrapper.hpp"
#include "constants/default_values.hpp"
#include "constants/request_code.hpp"
#include "constants/response_packet.hpp"
#include "logger/logger.hpp"
#include "nlohmann/json.hpp"
#include "plog/include/plog/Log.h"
#include "plog/include/plog/Appenders/ColorConsoleAppender.h"

#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <thread>

namespace server {

ResponsePacket ServerEngine::initServer(std::string path) {
	if (state_ != State::INSTANCIED) {
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_INVALID_STATE, .err_server_description = "Server already initialized" };
		return response_packet;
	}

	socket_ = new ServerTCPSocket();
	if ((path.size() > 1) && (path.at(0) == '{'))
	{
		config_.initFromJson(path);
	}
	else
	{
		config_.init(path);
	}
	logger::setup(&config_);

	// launch engine
	LOG_INFO << "Server launched";
	state_ = State::INITIALIZED;
	ResponsePacket response_packet;
	return response_packet;
}


ResponsePacket ServerEngine::startListening(const char* ip, const char* port) {
	if (state_ != State::INITIALIZED && state_ != State::DISCONNECTED) {
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_INVALID_STATE, .err_server_description = "Server invalid state" };
		return response_packet;
	}

	// start the server
	if (!socket_->startServer(ip, port)) {
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_NETWORK, .err_server_description = "Failed to start server" };
		return response_packet;
	}
	state_ = State::STARTED;
	stop_ = false;
	LOG_INFO << "Start listening on IP " << ip << " and port " << port;

	// launch a thread to handle incoming connections
	std::thread thr(&ServerEngine::handleConnections, this);
	std::swap(thr, connection_thread_);

	ResponsePacket response_packet;
	return response_packet;
}

ResponsePacket ServerEngine::handleConnections() {
	int default_timeout = std::atoi(config_.getValue("timeout", DEFAULT_SOCKET_TIMEOUT).c_str());
	std::future<ResponsePacket> future_connection;
	while (!stop_.load()) {
		SOCKET client_socket = INVALID_SOCKET;

		// accept incoming connection
		if (!socket_->acceptConnection(&client_socket, default_timeout)) {
			ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_NETWORK, .err_server_description = "Connection with client failed" };
			return response_packet;
		}

		// launch a thread to perform the connection handshake
		future_connection = std::async(std::launch::async, &ServerEngine::connectionHandshake, this, client_socket);
	}

	ResponsePacket response_packet;
	return response_packet;
}

ResponsePacket ServerEngine::connectionHandshake(SOCKET client_socket) {
	ResponsePacket response_packet;
	char client_name[DEFAULT_BUFLEN];

	if (!(socket_->receivePacket(client_socket, client_name)==RES_SOCKET_OK)) {
		LOG_INFO << "Handshake with client failed";
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_NETWORK, .err_server_description = "Network error on receive" };
		return response_packet;
	}

	ClientData* client = new ClientData(client_socket, ++next_client_id_, client_name);
	LOG_INFO << "Client connected [id:" << client->getId() << "][name:" << client->getName() << "]";
	if (notifyConnectionAccepted_ != 0)  {
		notifyConnectionAccepted_(client->getId(), client->getName().c_str());
	}

    std::lock_guard<std::mutex> guard(insert_client_mutex_);
	clients_.insert(std::make_pair(client->getId(), client));

	return response_packet;
}

ResponsePacket ServerEngine::handleRequest(int id_client, RequestCode request, bool isExpectedRes, DWORD request_timeout, std::string data) {
	if (state_ != State::STARTED) {
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_INVALID_STATE, .err_server_description = "Server must be started" };
		return response_packet;
	}

	if (clients_.find(id_client) == clients_.end()) {
		LOG_DEBUG << "Failed to retrieve client [id_client:" << id_client << "][request:" << requestCodeToString(request) << "]";
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_CLIENT_CLOSED, .err_server_description = "Client closed or not found" };
		return response_packet;
	}

	SOCKET client_socket = clients_.at(id_client)->getSocket();

	nlohmann::json j;
	j["request"] = request;
	j["data"] = data;
	j["timeout"] = request_timeout;

	DWORD socket_timeout = std::atoi(config_.getValue("timeout", DEFAULT_SOCKET_TIMEOUT).c_str());

	if (socket_timeout < (request_timeout + DEFAULT_ADDED_TIME))
	{
		LOG_DEBUG << "Socket timeout adapted. Previous value of socket_timeout:" << socket_timeout << ". Changed to " << (request_timeout + DEFAULT_ADDED_TIME) << ".]";
		socket_timeout = request_timeout + DEFAULT_ADDED_TIME;
	}
	// sends async request to client
	auto future = std::async(std::launch::async, &ServerEngine::asyncRequest, this, client_socket, j.dump(), socket_timeout, isExpectedRes);
	// blocks until the timeout has elapsed or the result became available
	if (future.wait_for(std::chrono::milliseconds(socket_timeout)) == std::future_status::timeout) {
		// thread has timed out
		LOG_DEBUG << "Response time from client has elapsed [client_socket:" << client_socket << "][request:" << j.dump() << "[timeout:" << request_timeout << "]";
		pending_futures_.push_back(std::move(future));
		for (long long unsigned int i = 0; i < pending_futures_.size(); i++) {
			if (pending_futures_[i].wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
				pending_futures_.erase(pending_futures_.begin() + i);
			}
		}
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_TIMEOUT, .err_server_description = "Request time elapsed" };
		return response_packet;
	}
	return future.get();
}

ResponsePacket ServerEngine::asyncRequest(SOCKET client_socket, std::string to_send, DWORD socket_timeout, bool isExpectedRes) {
	char recvbuf[DEFAULT_BUFLEN];
	nlohmann::json jresponse;
	int ret = 0;

	if (!socket_->sendPacket(client_socket, to_send.c_str())) {
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_NETWORK, .err_server_description = "Network error on send request" };
	}
	LOG_INFO << "Data sent to client: " << to_send.c_str();

	do {
		ret = socket_->receivePacket(client_socket, recvbuf);
		if (ret == RES_SOCKET_ERROR) {
			ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_NETWORK, .err_server_description = "Network error on receive" };
			return response_packet;
		} else if (ret == RES_SOCKET_WARNING){
			LOG_INFO << "SOCKET Warning Ignored, relaunch waiting socket reception";
			if (!isExpectedRes){
				ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_NETWORK, .err_server_description = "Network error on receive" };
				return response_packet;
			}
		}

	} while ((ret != RES_SOCKET_OK) && isExpectedRes);

	LOG_DEBUG << "Data received from client: " << recvbuf;

	try {
		jresponse = nlohmann::json::parse(recvbuf); // parses response to json object
	} catch (json::parse_error &err) {
		LOG_DEBUG << "Error while parsing the response [recvbuf:" << recvbuf << "]";
		ResponsePacket response_packet = { .response = "KO", .err_client_code = ERR_JSON_PARSING, .err_client_description = "Error while parsing the request" };
		return response_packet;
	}

	ResponsePacket response_packet = jresponse.get<ResponsePacket>();
	LOG_INFO << "Data received from client" << "{response: " << response_packet.response << ", err_server_code: " << response_packet.err_server_code << ", err_server_description: " << response_packet.err_server_description << ", err_client_code: " << response_packet.err_client_code << ", err_client_description: " << response_packet.err_client_description << ", err_terminal_code: " << response_packet.err_terminal_code << ", err_terminal_description: " << response_packet.err_terminal_description << ", err_card_code: " << response_packet.err_card_code << ", err_card_description: " << response_packet.err_card_description << "}";
	return response_packet;
}

ResponsePacket ServerEngine::listClients() {
	if (state_ != State::STARTED) {
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_INVALID_STATE, .err_server_description = "Server must be started" };
		return response_packet;
	}

	std::string output = "Clients connected: " +  std::to_string(clients_.size()) + "|";
	for (const auto &p : clients_) {
		output += std::to_string(p.second->getId()) + "|" + p.second->getName() + "|";
	}

	ResponsePacket response_packet = { .response = output };
	return response_packet;
}

ResponsePacket ServerEngine::stopAllClients() {
	if (state_ != State::STARTED) {
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_INVALID_STATE, .err_server_description = "Server must be started" };
		return response_packet;
	}

	stop_ = true; // stop active threads
	socket_->closeServer();
	connection_thread_.join();

	for (const auto &p : clients_) {
		stopClient(p.first);
	}

	state_ = State::DISCONNECTED;
	ResponsePacket response_packet;
	return response_packet;
}

ResponsePacket ServerEngine::stopClient(int id_client) {
	if (state_ != State::STARTED) {
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_INVALID_STATE, .err_server_description = "Server must be started" };
		return response_packet;
	}

	if (clients_.find(id_client) == clients_.end()) {
		LOG_DEBUG << "Failed to retrieve client [id_client:" << id_client << "]";
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_CLIENT_CLOSED, .err_server_description = "Client closed or not found" };
		return response_packet;
	}

	SOCKET client_socket = clients_.at(id_client)->getSocket();
	ResponsePacket response_packet = handleRequest(id_client, REQ_DISCONNECT, false);
	if (response_packet.err_server_code  < 0) {
		return response_packet;
	}

	if (shutdown(client_socket, SD_SEND) == SOCKET_ERROR) {
		LOG_DEBUG << "Failed to shutdown client [client_socket:" << client_socket << "][how:" << SD_SEND << "]";
		ResponsePacket response_packet = { .response = "KO", .err_server_code = ERR_NETWORK, .err_server_description = "Client shutdown failed" };
		return response_packet;
	}

	closesocket(client_socket);
	delete clients_.at(id_client);
	clients_.erase(id_client);

	return response_packet;
}

} /* namespace server */
