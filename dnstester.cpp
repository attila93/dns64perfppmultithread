/* dns64perf++ - C++11 DNS64 performance tester
 * Based on dns64perf by Gabor Lencse <lencse@sze.hu> (http://ipv6.tilb.sze.hu/dns64perf/)
 * Copyright (C) 2015  Daniel Bakai <bakaid@kszk.bme.hu>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
 
#include "dnstester.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <iostream>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <limits.h>

TestException::TestException(std::string what): what_{what} {}

const char* TestException::what() const noexcept {
	return what_.c_str();
}

DnsQuery::DnsQuery(): received_{false},
					  answered_{false}
{
}

DnsTester::DnsTester(struct in6_addr server_addr, uint16_t port, uint32_t ip, uint8_t netmask, uint32_t num_req, uint32_t num_burst, std::chrono::nanoseconds burst_delay):
					ip_{ip},
					netmask_{netmask},
					num_req_{num_req},
					num_burst_{num_burst},
					burst_delay_{burst_delay},
					num_sent_{0}
	{
	/* Fill server sockaddr structure */
	memset(&server_, 0x00, sizeof(server_));
	server_.sin6_family = AF_INET6;
	server_.sin6_addr = server_addr;
	server_.sin6_port = htons(port);
	/* Create socket */
	int sockfd;
	if ((sockfd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		std::stringstream ss;
		ss << "Cannot create socket: " << strerror(errno);
		throw TestException{ss.str()};
	}
	sock_ = Socket{sockfd};
	/* Bind socket */
	struct sockaddr_in6 local_addr;
	memset(&local_addr, 0x00, sizeof(local_addr));
	local_addr.sin6_family = AF_INET6;  // IPv6
	local_addr.sin6_addr = in6addr_any; // To any valid IP address
	local_addr.sin6_port = htons(0);   // Get a random port
	if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&local_addr), sizeof(local_addr)) == -1) {
		std::stringstream ss;
		ss << "Unable to bind socket: " << strerror(errno);
		throw TestException{ss.str()};
	}
	/* Set socket timeout */
	struct timeval tv;
	tv.tv_sec = recvfrom_timeout;
	tv.tv_usec = 0;
	if (::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const void*>(&tv), sizeof(tv))) {
		throw TestException("Cannot set timeout: setsockopt failed");
	}
	/* Preallocate the test queries */
	tests_.reserve(num_req_);
	/* Create the test queries */
	for (int i = 0; i < num_req_; i++) {
		tests_.push_back(DnsQuery{});
	}
	/* Creating the base query */
	memset(query_data_, 0x00, sizeof(query_data_));
	/* Filling the header */
	DNSHeader* header = reinterpret_cast<DNSHeader*>(query_data_);
	header->id(0);
	header->qr(0);
	header->opcode(DNSHeader::OpCode::Query);
	header->aa(false);
	header->tc(false);
	header->rd(true);
	header->ra(false);
	header->rcode(DNSHeader::RCODE::NoError);
	header->qdcount(1);
	header->ancount(0);
	header->nscount(0);
	header->arcount(0);
	/* Creating the question*/
	uint8_t* question = query_data_ + sizeof(DNSHeader);
	/* Creating the domain name */
	char addr[64];
	char query_addr[512];
	snprintf(addr, sizeof(addr), dns64_addr_format_string, 0, 0, 0, 0);
	snprintf(query_addr, sizeof(query_addr), "%s.%s.", addr, dns64_addr_domain);
	/* Convering the domain name to DNS Name format */
	char* label = strtok(query_addr, ".");
	while (label != nullptr) {
		*question = strlen(label);
		question += 1;
		memcpy(question, label, strlen(label));
		question += strlen(label);
		label = strtok(nullptr, ".");
	}
	*question = 0x00;
	question += 1;
	/* Setting the query type and class */
	*reinterpret_cast<uint16_t*>(question) = htons(QType::AAAA);
	question += sizeof(uint16_t);
	*reinterpret_cast<uint16_t*>(question) = htons(QClass::IN);
	question += sizeof(uint16_t);
	/* Constructing the DnsQuery */
	size_t len = (size_t) (question - query_data_);
	query_ = std::unique_ptr<DNSPacket>{new DNSPacket{query_data_, len, sizeof(query_data_)}};
}

void DnsTester::test() {
	for (int i = 0; i < num_burst_; i++) {
		m_.lock();
		/* Get query store */
		DnsQuery& query = tests_[num_sent_];
		/* Modify the base query */
		char label[64];
		uint32_t ip = ip_ | num_sent_;
		snprintf(label, sizeof(label), dns64_addr_format_string, (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
		memcpy(query_->labels_[0].begin_+1, label, strlen(label));
		m_.unlock();
		/* Send the query */
		if (::sendto(sock_, reinterpret_cast<const void*>(query_->begin_), query_->len_, 0, reinterpret_cast<const struct sockaddr*>(&server_), sizeof(server_)) != query_->len_) {
			std::cerr << "Can't send packet." << std::endl;
		}
		/* Store the time */
		query.time_sent_ = std::chrono::high_resolution_clock::now();
		m_.lock();
		num_sent_++;
		m_.unlock();
	}
}

void DnsTester::start() {
	/* Starting test packet sending */
	timer_ = std::unique_ptr<Timer>{new Timer{std::bind(&DnsTester::test, this), burst_delay_, (size_t) (num_req_ / num_burst_)}};
	timer_->start();
	/* Receiving answers */
	struct sockaddr_in6 sender;
	socklen_t sender_len;
	ssize_t recvlen;
	uint8_t answer_data[UDP_MAX_LEN];
	bool continue_receiving;
	
	continue_receiving = true;
	while (continue_receiving) {
		m_.lock();
		size_t remaining = num_req_ - num_sent_;
		m_.unlock();
		if (remaining == 0) {
			continue_receiving = false;
		}
		memset(&sender, 0x00, sizeof(sender));
		sender_len = sizeof(sender);
		if ((recvlen = ::recvfrom(sock_, answer_data, sizeof(answer_data), 0, reinterpret_cast<struct sockaddr*>(&sender), &sender_len)) > 0) {
			/* Get the time of the receipt */
			std::chrono::high_resolution_clock::time_point time_received = std::chrono::high_resolution_clock::now();
			/* Test whether the answer came from the DUT */
			if (memcmp(reinterpret_cast<const void*>(&sender.sin6_addr), reinterpret_cast<const void*>(&server_.sin6_addr), sizeof(struct in6_addr)) != 0 || sender.sin6_port != server_.sin6_port) {
				char sender_text[INET6_ADDRSTRLEN];
				inet_ntop(AF_INET6, reinterpret_cast<const void*>(&sender.sin6_addr), sender_text, sizeof(sender_text));
				std::stringstream ss;
				ss << "Received packet from other host than the DUT: [" << sender_text << "]:" << ntohs(sender.sin6_port);
				throw TestException{ss.str()};
			}
			/* Parse the answer */
			DNSPacket answer{answer_data, (size_t) recvlen, sizeof(answer_data)};
			/* Test whether the query is valid */
			if (answer.header_->qdcount() < 1) {
				throw TestException{"Invalid answer from server, qdcount == 0"};
			}
			/* Find the corresponding query */
			char label[64];
			uint32_t ip;
			uint8_t temp[4];
			strncpy(label, (const char*) answer.labels_[0].begin_ + 1, answer.labels_[0].length());
			label[answer.labels_[0].length()] = '\0';
			if (sscanf(label, dns64_addr_format_string, temp, temp+1, temp+2, temp+3) != 4) {
				throw TestException{"Invalid question."};
			}
			ip = (temp[0] << 24) | (temp[1] << 16) | (temp[2] << 8) | temp[3];
			if ((ip & ((1 << (32-netmask_))-1)) >= num_req_) {
				throw TestException{"Unexpected FQDN in question: too large."};
			}
			m_.lock();
			DnsQuery& query = tests_[(ip & (((uint64_t) 1 << (32-netmask_))-1))];
			m_.unlock();
			/* Set the received flag true */
			query.received_ = true;
			/* Calculate the Round-Trip-Time */
			query.rtt_ = std::chrono::duration_cast<std::chrono::nanoseconds>(time_received - query.time_sent_);
			/* Check whether there is an answer */
			query.answered_ = answer.header_->qr() == 1 && answer.header_->rcode() == DNSHeader::RCODE::NoError && answer.header_->ancount() > 0;
		} else {
			/* If the error is not caused by timeout, there is something wrong */
			if (errno != EWOULDBLOCK) {
				std::stringstream ss;
				ss << "Error in recvfrom: " << strerror(errno);
				throw TestException{ss.str()};
			}
		}
	}
}

void DnsTester::display() {
	uint16_t num_received, num_answered;
	double average, standard_deviation;
	num_received = 0;
	num_answered = 0;
	/* Number of received and answered queries */
	for (auto& query: tests_) {
		if (query.received_) {
			num_received++;
		}
		if (query.answered_) {
			num_answered++;
		}
	}
	/* Average */
	average = 0;
	for (auto& query: tests_) {
		if (query.received_) {
			average += (double) query.rtt_.count() / num_received;
		}
	}
	/* Standard deviation */
	standard_deviation = 0;
	for (auto& query: tests_) {
		if (query.received_) {
			standard_deviation += pow(query.rtt_.count() - average, 2.0);
		}
	}
	standard_deviation = sqrt(standard_deviation / num_received);
	/* Print results */
	printf("Sent queries: %zu\n", tests_.size());
	printf("Received answers: %hu (%.02f%%)\n", num_received, ((double) num_received / tests_.size()) * 100);
	printf("Valid answers: %hu (%.02f%%)\n", num_answered, (double) num_answered / tests_.size());
	printf("Average round-trip time: %.02f ms\n", average / 1000000.0);
	printf("Standard deviation of the round-trip time: %.02f ms\n", standard_deviation / 1000000.0);
}

void DnsTester::write(const char* filename) {
	FILE* fp;
	char server[INET6_ADDRSTRLEN];
	/* Convert server address to string */
	if (inet_ntop(AF_INET6, reinterpret_cast<const void*>(&server_.sin6_addr), server, sizeof(server)) == NULL) {
		std::stringstream ss;
		ss << "Bad server address: " << strerror(errno);
		throw TestException{ss.str()};
	}
	/* Open file */
	if ((fp = fopen(filename, "w")) == nullptr) {
		throw TestException{"Can't open file"};
	}
	/* Write header */
	fprintf(fp, "%s\n", "dns64perf++ test parameters");
	fprintf(fp, "server: %s\n", server);
	fprintf(fp, "port: %hu\n", ntohs(server_.sin6_port));
	fprintf(fp, "number of requests: %u\n", num_req_);
	fprintf(fp, "burst size: %u\n", num_burst_);
	fprintf(fp, "delay between bursts: %lu ns\n\n", burst_delay_.count());
	fprintf(fp, "query;tsent [ns];received;answered;rtt [ns]\n");
	/* Write queries */
	char addr[64];
	char query_addr[512];
	uint32_t ip;
	int n = 0;
	for (auto& query: tests_) {
		ip = ip_ | n++;
		snprintf(addr, sizeof(addr), dns64_addr_format_string, (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
		snprintf(query_addr, sizeof(query_addr), "%s.%s.", addr, dns64_addr_domain);
		fprintf(fp, "%s;%lu;%d;%d;%lu\n", query_addr, std::chrono::duration_cast<std::chrono::nanoseconds>(query.time_sent_.time_since_epoch()).count(), query.received_, query.answered_, query.rtt_.count());
	}
	fclose(fp);
}
