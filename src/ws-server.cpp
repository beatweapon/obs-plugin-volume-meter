#include "ws-server.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t invalid_socket_value = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t invalid_socket_value = -1;
#endif

namespace {
std::atomic<bool> running(false);
std::thread server_thread;
std::mutex clients_mutex;
std::vector<socket_t> clients;
socket_t listen_socket = invalid_socket_value;
unsigned short current_port = 0;

void close_socket(socket_t socket)
{
	if (socket == invalid_socket_value)
		return;
#ifdef _WIN32
	closesocket(socket);
#else
	close(socket);
#endif
}

std::string base64_encode(const uint8_t *data, size_t length)
{
	static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string encoded;
	encoded.reserve(((length + 2) / 3) * 4);

	for (size_t i = 0; i < length; i += 3) {
		uint32_t value = data[i] << 16;
		if (i + 1 < length)
			value |= data[i + 1] << 8;
		if (i + 2 < length)
			value |= data[i + 2];

		encoded.push_back(table[(value >> 18) & 0x3f]);
		encoded.push_back(table[(value >> 12) & 0x3f]);
		encoded.push_back(i + 1 < length ? table[(value >> 6) & 0x3f] : '=');
		encoded.push_back(i + 2 < length ? table[value & 0x3f] : '=');
	}

	return encoded;
}

uint32_t left_rotate(uint32_t value, uint32_t bits)
{
	return (value << bits) | (value >> (32 - bits));
}

std::array<uint8_t, 20> sha1(const std::string &message)
{
	std::vector<uint8_t> bytes(message.begin(), message.end());
	const uint64_t bit_length = static_cast<uint64_t>(bytes.size()) * 8;
	bytes.push_back(0x80);
	while ((bytes.size() % 64) != 56)
		bytes.push_back(0);
	for (int i = 7; i >= 0; --i)
		bytes.push_back(static_cast<uint8_t>((bit_length >> (i * 8)) & 0xff));

	uint32_t h0 = 0x67452301;
	uint32_t h1 = 0xefcdab89;
	uint32_t h2 = 0x98badcfe;
	uint32_t h3 = 0x10325476;
	uint32_t h4 = 0xc3d2e1f0;

	for (size_t chunk = 0; chunk < bytes.size(); chunk += 64) {
		uint32_t words[80] = {};
		for (int i = 0; i < 16; ++i) {
			const size_t offset = chunk + i * 4;
			words[i] = (bytes[offset] << 24) | (bytes[offset + 1] << 16) | (bytes[offset + 2] << 8) |
				   bytes[offset + 3];
		}
		for (int i = 16; i < 80; ++i)
			words[i] = left_rotate(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);

		uint32_t a = h0;
		uint32_t b = h1;
		uint32_t c = h2;
		uint32_t d = h3;
		uint32_t e = h4;

		for (int i = 0; i < 80; ++i) {
			uint32_t f = 0;
			uint32_t k = 0;
			if (i < 20) {
				f = (b & c) | ((~b) & d);
				k = 0x5a827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ed9eba1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdc;
			} else {
				f = b ^ c ^ d;
				k = 0xca62c1d6;
			}

			const uint32_t temp = left_rotate(a, 5) + f + e + k + words[i];
			e = d;
			d = c;
			c = left_rotate(b, 30);
			b = a;
			a = temp;
		}

		h0 += a;
		h1 += b;
		h2 += c;
		h3 += d;
		h4 += e;
	}

	std::array<uint8_t, 20> digest = {};
	const uint32_t hashes[] = {h0, h1, h2, h3, h4};
	for (size_t i = 0; i < 5; ++i) {
		digest[i * 4] = static_cast<uint8_t>((hashes[i] >> 24) & 0xff);
		digest[i * 4 + 1] = static_cast<uint8_t>((hashes[i] >> 16) & 0xff);
		digest[i * 4 + 2] = static_cast<uint8_t>((hashes[i] >> 8) & 0xff);
		digest[i * 4 + 3] = static_cast<uint8_t>(hashes[i] & 0xff);
	}
	return digest;
}

std::string header_value(const std::string &request, const std::string &name)
{
	const std::string key = name + ":";
	size_t pos = request.find(key);
	if (pos == std::string::npos)
		return {};
	pos += key.size();
	while (pos < request.size() && (request[pos] == ' ' || request[pos] == '\t'))
		++pos;
	const size_t end = request.find("\r\n", pos);
	return request.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

bool websocket_handshake(socket_t client)
{
	char buffer[4096] = {};
	const int received = recv(client, buffer, sizeof(buffer) - 1, 0);
	if (received <= 0)
		return false;

	const std::string request(buffer, static_cast<size_t>(received));
	const std::string key = header_value(request, "Sec-WebSocket-Key");
	if (key.empty())
		return false;

	const std::string accept_src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	const auto digest = sha1(accept_src);
	const std::string accept = base64_encode(digest.data(), digest.size());
	const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
				     "Upgrade: websocket\r\n"
				     "Connection: Upgrade\r\n"
				     "Sec-WebSocket-Accept: " +
				     accept + "\r\n\r\n";

	return send(client, response.c_str(), static_cast<int>(response.size()), 0) == static_cast<int>(response.size());
}

void server_loop(unsigned short port)
{
	listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_socket == invalid_socket_value)
		return;

	int enabled = 1;
	setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&enabled), sizeof(enabled));

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);

	if (bind(listen_socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
	    listen(listen_socket, SOMAXCONN) != 0) {
		close_socket(listen_socket);
		listen_socket = invalid_socket_value;
		return;
	}

	while (running.load()) {
		fd_set read_set;
		FD_ZERO(&read_set);
		FD_SET(listen_socket, &read_set);
		timeval timeout = {};
		timeout.tv_sec = 0;
		timeout.tv_usec = 250000;

		const int selected = select(static_cast<int>(listen_socket + 1), &read_set, nullptr, nullptr, &timeout);
		if (selected <= 0)
			continue;

		socket_t client = accept(listen_socket, nullptr, nullptr);
		if (client == invalid_socket_value)
			continue;

		if (!websocket_handshake(client)) {
			close_socket(client);
			continue;
		}

		std::lock_guard<std::mutex> lock(clients_mutex);
		clients.push_back(client);
	}

	close_socket(listen_socket);
	listen_socket = invalid_socket_value;
}

std::vector<uint8_t> make_text_frame(const char *text)
{
	const size_t length = std::strlen(text);
	std::vector<uint8_t> frame;
	frame.reserve(length + 10);
	frame.push_back(0x81);
	if (length < 126) {
		frame.push_back(static_cast<uint8_t>(length));
	} else if (length <= 0xffff) {
		frame.push_back(126);
		frame.push_back(static_cast<uint8_t>((length >> 8) & 0xff));
		frame.push_back(static_cast<uint8_t>(length & 0xff));
	} else {
		frame.push_back(127);
		for (int i = 7; i >= 0; --i)
			frame.push_back(static_cast<uint8_t>((length >> (i * 8)) & 0xff));
	}
	frame.insert(frame.end(), text, text + length);
	return frame;
}
}

bool ws_server_start(unsigned short port)
{
	if (running.exchange(true))
		return true;

#ifdef _WIN32
	WSADATA data = {};
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
		running.store(false);
		return false;
	}
#endif

	current_port = port;
	server_thread = std::thread(server_loop, port);
	return true;
}

void ws_server_stop(void)
{
	if (!running.exchange(false))
		return;

	if (server_thread.joinable())
		server_thread.join();

	std::lock_guard<std::mutex> lock(clients_mutex);
	for (socket_t client : clients)
		close_socket(client);
	clients.clear();

#ifdef _WIN32
	WSACleanup();
#endif
}

void ws_server_broadcast_text(const char *text)
{
	if (!text)
		return;

	const auto frame = make_text_frame(text);
	std::lock_guard<std::mutex> lock(clients_mutex);
	for (auto it = clients.begin(); it != clients.end();) {
		const int sent = send(*it, reinterpret_cast<const char *>(frame.data()), static_cast<int>(frame.size()), 0);
		if (sent != static_cast<int>(frame.size())) {
			close_socket(*it);
			it = clients.erase(it);
		} else {
			++it;
		}
	}
}

unsigned short ws_server_port(void)
{
	return current_port;
}
