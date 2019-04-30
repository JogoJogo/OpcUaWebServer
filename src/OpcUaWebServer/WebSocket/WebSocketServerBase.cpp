/*
   Copyright 2015-2019 Kai Huebl (kai@huebl-sgh.de)

   Lizenziert gemäß Apache Licence Version 2.0 (die „Lizenz“); Nutzung dieser
   Datei nur in Übereinstimmung mit der Lizenz erlaubt.
   Eine Kopie der Lizenz erhalten Sie auf http://www.apache.org/licenses/LICENSE-2.0.

   Sofern nicht gemäß geltendem Recht vorgeschrieben oder schriftlich vereinbart,
   erfolgt die Bereitstellung der im Rahmen der Lizenz verbreiteten Software OHNE
   GEWÄHR ODER VORBEHALTE – ganz gleich, ob ausdrücklich oder stillschweigend.

   Informationen über die jeweiligen Bedingungen für Genehmigungen und Einschränkungen
   im Rahmen der Lizenz finden Sie in der Lizenz.

   Autor: Kai Huebl (kai@huebl-sgh.de)

 */

#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include "OpcUaStackCore/Base/Log.h"
#include "OpcUaWebServer/WebSocket/WebSocketServerBase.h"

using namespace OpcUaStackCore;

namespace OpcUaWebServer
{

	WebSocketServerBase::WebSocketServerBase(WebSocketConfig* webSocketConfig)
	: webSocketConfig_(webSocketConfig)
	, tcpAcceptor_(webSocketConfig->ioThread()->ioService()->io_service(), webSocketConfig->address(), webSocketConfig->port())
	, receiveMessageCallback_()
	, webSocketChannelMap_()
	, channelId_(0)
	, mutex_()
	{
	}

	WebSocketServerBase::~WebSocketServerBase(void)
	{
	}

	void
	WebSocketServerBase::disconnect(uint32_t channelId)
	{
		WebSocketChannel* webSocketChannel;

		boost::mutex::scoped_lock g(mutex_);

		// get web socket channel
		auto it = webSocketChannelMap_.find(channelId);
		if (it == webSocketChannelMap_.end()) {
			Log(Error, "web socket channel not exist")
				.parameter("ChannelId", channelId);
			return;
		}
		webSocketChannel = it->second;

		closeWebSocketChannel(webSocketChannel);
	}

	void
	WebSocketServerBase::receiveMessageCallback(
		const ReceiveMessageCallback& receiveMessageCallback
	)
	{
		receiveMessageCallback_ = receiveMessageCallback;
	}

	bool
	WebSocketServerBase::sendMessage(WebSocketMessage& webSocketMessage)
	{
		WebSocketChannel* webSocketChannel;

		{
			boost::mutex::scoped_lock g(mutex_);

			// get web socket channel
			auto it = webSocketChannelMap_.find(webSocketMessage.channelId_);
			if (it == webSocketChannelMap_.end()) {
				Log(Error, "web socket channel not exist")
					.parameter("ChannelId", webSocketMessage.channelId_);
				return false;
			}
			webSocketChannel = it->second;
		}

		// send message to client
		return sendMessage(webSocketMessage, webSocketChannel);
	}

	void
	WebSocketServerBase::closeWebSocketChannel(WebSocketChannel* webSocketChannel)
	{
		// remove web socket from channel map
		cleanupWebSocketChannel(webSocketChannel);

		// send channel close message to opc ua client
		WebSocketMessage webSocketMessage;
		webSocketMessage.channelId_ = webSocketChannel->id_;
		webSocketMessage.message_ = "{\"Header\":{\"MessageType\": \"CHANNELCLOSE_MESSAGE\",\"ClientHandle\": \"---\"},\"Body\":{}}";
		if (receiveMessageCallback_) receiveMessageCallback_(webSocketMessage);

		// close and delete channel
		webSocketChannel->close();
		delete webSocketChannel;
	}

	// ------------------------------------------------------------------------
	// ------------------------------------------------------------------------
	//
	// web socket managament
	//
	// ------------------------------------------------------------------------
	// ------------------------------------------------------------------------
	void
	WebSocketServerBase::initWebSocketChannel(WebSocketChannel* webSocketChannel)
	{
		boost::mutex::scoped_lock g(mutex_);

		channelId_++;
		webSocketChannel->channelId_ = channelId_;
		webSocketChannelMap_.insert(std::make_pair(channelId_, webSocketChannel));
	}

	void
	WebSocketServerBase::cleanupWebSocketChannel(WebSocketChannel* webSocketChannel)
	{
		boost::mutex::scoped_lock g(mutex_);

		WebSocketChannel::Map::iterator it;
		it = webSocketChannelMap_.find(webSocketChannel->id_);
		if (it != webSocketChannelMap_.end()) webSocketChannelMap_.erase(it);
	}

	// ------------------------------------------------------------------------
	// ------------------------------------------------------------------------
	//
	// handle handshake
	//
	// ------------------------------------------------------------------------
	// ------------------------------------------------------------------------
	void
	WebSocketServerBase::receiveHandshake(WebSocketChannel* webSocketChannel)
	{
		// start timer
		webSocketChannel->slotTimerElement_->expireFromNow(webSocketConfig_->requestTimeout());
		webSocketChannel->slotTimerElement_->callback().reset(boost::bind(&WebSocketServerBase::handleReceiveHandshakeHeaderTimeout, this, webSocketChannel));
		webSocketConfig_->ioThread()->slotTimer()->start(webSocketChannel->slotTimerElement_);

		webSocketChannel->async_read_until(
			webSocketChannel->recvBuffer_,
			boost::bind(&WebSocketServerBase::handleReceiveHandshakeHeader, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, webSocketChannel),
			"\r\n\r\n"
		);
	}

	void
	WebSocketServerBase::handleReceiveHandshakeHeaderTimeout(WebSocketChannel* webSocketChannel)
	{
		requestTimeoutWebSocketChannel(webSocketChannel, "request header");
	}

	void
	WebSocketServerBase::handleReceiveHandshakeHeader(const boost::system::error_code& error, std::size_t bytes_transfered, WebSocketChannel* webSocketChannel)
	{
		if (webSocketChannel->timeout_) {
			closeWebSocketChannel(webSocketChannel);
			return;
		}

		// stop request timer
		webSocketConfig_->ioThread()->slotTimer()->stop(webSocketChannel->slotTimerElement_);

		if (error) {
			Log(Debug, "WebSocketServer receive request header error; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		size_t numAdditionalBytes = webSocketChannel->recvBuffer_.size() - bytes_transfered;
		std::istream is(&webSocketChannel->recvBuffer_);

		//
		// read request header
		//
		if (!webSocketChannel->webSocketRequest_.decodeRequestHeader(is)) {
			Log(Debug, "WebSocketServer decode request error; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port());

			closeWebSocketChannel(webSocketChannel);
		}

		//
		// read request content
		//
		std::string contentLengthString;
		if (!webSocketChannel->webSocketRequest_.getHeaderPara("Content-Length", contentLengthString)) {
			processHandshake(webSocketChannel, webSocketChannel->webSocketRequest_);
			return;
		}

		bool success = true;
		size_t contentLength;
		try {
			contentLength = boost::lexical_cast<size_t>(contentLengthString);
		} catch(boost::bad_lexical_cast&) {
			success = false;
			return;
		}

		if (!success) {
			Log(Debug, "WebSocketServer content length error in request; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port());

			closeWebSocketChannel(webSocketChannel);
		}

		// start request timer
		webSocketChannel->slotTimerElement_->expireFromNow(webSocketConfig_->requestTimeout());
		webSocketChannel->slotTimerElement_->callback().reset(boost::bind(&WebSocketServerBase::handleReceiveHandshakeContentTimeout, this, webSocketChannel));
		webSocketConfig_->ioThread()->slotTimer()->start(webSocketChannel->slotTimerElement_);

		webSocketChannel->async_read_exactly(
			webSocketChannel->recvBuffer_,
			boost::bind(&WebSocketServerBase::handleReceiveHandshakeContent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, webSocketChannel),
			contentLength-numAdditionalBytes
		);
	}

	void
	WebSocketServerBase::handleReceiveHandshakeContentTimeout(WebSocketChannel* webSocketChannel)
	{
		requestTimeoutWebSocketChannel(webSocketChannel, "request content");
	}

	void
	WebSocketServerBase::handleReceiveHandshakeContent(const boost::system::error_code& error, std::size_t bytes_transfered, WebSocketChannel* webSocketChannel)
	{
		if (webSocketChannel->timeout_) {
			closeWebSocketChannel(webSocketChannel);
			return;
		}

		// stop request timer
		webSocketConfig_->ioThread()->slotTimer()->stop(webSocketChannel->slotTimerElement_);

		if (error) {
			Log(Debug, "WebSocketServer receive request content error; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		//
		// read content
		//
		processHandshake(webSocketChannel, webSocketChannel->webSocketRequest_);
	}

	void
	WebSocketServerBase::processHandshake(WebSocketChannel* webSocketChannel, WebSocketRequest& webSocketRequest)
	{
		// determine request method
		std::string method = webSocketRequest.method();
		boost::to_upper(method);

		// HTTP/1.1 101 Web Socket Protocol Handshake\r\n
        // Upgrade: websocket\r\n
        // Connection: Upgrade\r\n
        // Sec-WebSocket-Accept: ...

		// get Sec-WebSocket-Key
		std::string webSocketKey;
		if (!webSocketRequest.getHeaderPara("Sec-WebSocket-Key", webSocketKey)) {
			Log(Debug, "WebSocketServer request without parameter Sec-WebSocket-key; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		const std::string ws_magic_string="258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		webSocketKey = webSocketKey + ws_magic_string;

		unsigned char key[20];
		SHA_CTX context;
		SHA1_Init(&context);
		SHA1_Update(&context, webSocketKey.c_str(), webSocketKey.length());
		SHA1_Final(key, &context);

		char* b64text;
		base64Encode(key, 20, &b64text);

		webSocketChannel->webSocketResponse_.statusCode(101);
		webSocketChannel->webSocketResponse_.statusString("Web Socket Protocol Handshake");
		webSocketChannel->webSocketResponse_.setHeaderPara("Upgrade", "websocket");
		webSocketChannel->webSocketResponse_.setHeaderPara("Connection", "Upgrade");
		webSocketChannel->webSocketResponse_.setHeaderPara("Sec-WebSocket-Accept", b64text);
		webSocketChannel->webSocketResponse_.setHeaderPara("Sec-WebSocket-Protocol", "json");
		delete b64text;

		// send response
		std::ostream os(&webSocketChannel->sendBuffer_);
		webSocketChannel->webSocketResponse_.encodeRequestHeader(os);
		webSocketChannel->async_write(
			webSocketChannel->sendBuffer_,
			boost::bind(&WebSocketServerBase::handleWriteComplete, this, boost::asio::placeholders::error, webSocketChannel)
		);

		return;
	}

	void
	WebSocketServerBase::handleWriteComplete(const boost::system::error_code& error, WebSocketChannel* webSocketChannel)
	{
		if (error) {
			Log(Debug, "WebSocketServer send response error; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		receiveMessage(webSocketChannel);
	}

	int
	WebSocketServerBase::base64Encode(const unsigned char* buffer, size_t length, char** b64text)
	{
		BIO *bio, *b64;
		BUF_MEM *bufferPtr;
		b64 = BIO_new(BIO_f_base64());
		bio = BIO_new(BIO_s_mem());
		bio = BIO_push(b64, bio);
		BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
		BIO_write(bio, buffer, length);
		BIO_flush(bio);
		BIO_get_mem_ptr(bio, &bufferPtr);
		BIO_set_close(bio, BIO_NOCLOSE);

		*b64text = (char*) malloc((bufferPtr->length + 1) * sizeof(char));
		memcpy(*b64text, bufferPtr->data, bufferPtr->length);
		(*b64text)[bufferPtr->length] = '\0';

		BIO_free_all(bio);
		return (0); //success
	}

	void
	WebSocketServerBase::requestTimeoutWebSocketChannel(WebSocketChannel* webSocketChannel, const std::string& location)
	{
		Log(Debug, "WebSocketServer request timeout; close channel")
			.parameter("Address", webSocketChannel->partner_.address().to_string())
			.parameter("Port", webSocketChannel->partner_.port())
			.parameter("Location", location)
			.parameter("ChannelId", webSocketChannel->channelId_);

		webSocketChannel->timeout_ = true;
		webSocketChannel->cancel();
	}

	// ------------------------------------------------------------------------
	// ------------------------------------------------------------------------
	//
	// handle receive messages
	//
	// ------------------------------------------------------------------------
	// ------------------------------------------------------------------------
	void
	WebSocketServerBase::receiveMessage(WebSocketChannel* webSocketChannel)
	{
		// start request timer
		webSocketChannel->slotTimerElement_->expireFromNow(webSocketConfig_->idleTimeout());
		webSocketChannel->slotTimerElement_->callback().reset(boost::bind(&WebSocketServerBase::handleIdleTimeout, this, webSocketChannel));
		webSocketConfig_->ioThread()->slotTimer()->start(webSocketChannel->slotTimerElement_);

		webSocketChannel->async_read_exactly(
			webSocketChannel->recvBuffer_,
			boost::bind(&WebSocketServerBase::handleReceiveMessageHeader, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, webSocketChannel),
			2
		);
	}

	void
	WebSocketServerBase::handleIdleTimeout(WebSocketChannel* webSocketChannel)
	{
		idleTimeoutWebSocketChannel(webSocketChannel, "json header");
	}

	void
	WebSocketServerBase::handleReceiveMessageHeader(const boost::system::error_code& error, std::size_t bytes_transfered, WebSocketChannel* webSocketChannel)
	{
		if (webSocketChannel->timeout_) {
			closeWebSocketChannel(webSocketChannel);
			return;
		}

		// stop idle timer
		webSocketConfig_->ioThread()->slotTimer()->stop(webSocketChannel->slotTimerElement_);

		if (error) {
			Log(Debug, "WebSocketServer receive message header error; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		std::istream is(&webSocketChannel->recvBuffer_);
		char headerBytes[2];
		is.read(headerBytes, 2);

		bool fin = (headerBytes[0] & 0x80) == 0x80;	// true - last frame
		uint8_t opcode = headerBytes[0] & 0x0F;		// 1 - text frame
		bool mask = (headerBytes[1] & 0x80) == 0x80;	// true - mask
		uint32_t length = headerBytes[1] & 0x7f;

		if (opcode == OP_CONTINUATION_FRAME) {
			Log(Debug, "WebSocketServer do not support continuation frame messages; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		if (opcode == OP_BINARY_FRAME) {
			Log(Error, "WebSocketServer do not support binary frame messages; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		if (opcode == OP_CLOSE_FRAME) {
			Log(Debug, "WebSocketServer receive close frame messages; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		if (opcode == OP_PING_FRAME) {
			Log(Debug, "WebSocketServer do not support continuation ping messages; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		if (opcode != OP_TEXT_FRAME) {
			Log(Debug, "WebSocketServer do not support continuation text messages; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		if (length <= 125) {
			// start request timer
			webSocketChannel->slotTimerElement_->expireFromNow(webSocketConfig_->requestTimeout());
			webSocketChannel->slotTimerElement_->callback().reset(boost::bind(&WebSocketServerBase::handleReceiveMessageContentTimeout, this, webSocketChannel));
			webSocketConfig_->ioThread()->slotTimer()->start(webSocketChannel->slotTimerElement_);

			webSocketChannel->async_read_exactly(
				webSocketChannel->recvBuffer_,
				boost::bind(&WebSocketServerBase::handleReceiveMessageContent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, webSocketChannel),
				length+4
			);
			return;
		}

		if (length == 126) {
			// start request timer
			webSocketChannel->slotTimerElement_->expireFromNow(webSocketConfig_->requestTimeout());
			webSocketChannel->slotTimerElement_->callback().reset(boost::bind(&WebSocketServerBase::handleReceiveMessageLength2Timeout, this, webSocketChannel));
			webSocketConfig_->ioThread()->slotTimer()->start(webSocketChannel->slotTimerElement_);

			webSocketChannel->async_read_exactly(
				webSocketChannel->recvBuffer_,
				boost::bind(&WebSocketServerBase::handleReceiveMessageLength2, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, webSocketChannel),
				2
			);
			return;
		}

		if (length == 127) {
			Log(Debug, "WebSocketServer do not support 8 byte length field; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

	}

	void
	WebSocketServerBase::handleReceiveMessageLength2Timeout(WebSocketChannel* webSocketChannel)
	{
		idleTimeoutWebSocketChannel(webSocketChannel, "json message length2");
	}

	void
	WebSocketServerBase::handleReceiveMessageLength2(const boost::system::error_code& error, std::size_t bytes_transfered, WebSocketChannel* webSocketChannel)
	{
		if (webSocketChannel->timeout_) {
			closeWebSocketChannel(webSocketChannel);
			return;
		}

		// stop request timer
		webSocketConfig_->ioThread()->slotTimer()->stop(webSocketChannel->slotTimerElement_);

		if (error) {
			Log(Debug, "WebSocketServer receive message content2 error; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		// read two byte length
		std::istream is(&webSocketChannel->recvBuffer_);
		char lengthBytes[2];
		is.read(lengthBytes, 2);

		uint32_t length = (lengthBytes[0] & 0xFF) * 256
				        + (lengthBytes[1] & 0xFF);

		// start request timer
		webSocketChannel->slotTimerElement_->expireFromNow(webSocketConfig_->requestTimeout());
		webSocketChannel->slotTimerElement_->callback().reset(boost::bind(&WebSocketServerBase::handleReceiveMessageContentTimeout, this, webSocketChannel));
		webSocketConfig_->ioThread()->slotTimer()->start(webSocketChannel->slotTimerElement_);

		webSocketChannel->async_read_exactly(
			webSocketChannel->recvBuffer_,
			boost::bind(&WebSocketServerBase::handleReceiveMessageContent, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred, webSocketChannel),
			length+4
		);
	}

	void
	WebSocketServerBase::handleReceiveMessageContentTimeout(WebSocketChannel* webSocketChannel)
	{
		idleTimeoutWebSocketChannel(webSocketChannel, "json content");
	}

	void
	WebSocketServerBase::handleReceiveMessageContent(const boost::system::error_code& error, std::size_t bytes_transfered, WebSocketChannel* webSocketChannel)
	{
		if (webSocketChannel->timeout_) {
			closeWebSocketChannel(webSocketChannel);
			return;
		}

		// stop request timer
		webSocketConfig_->ioThread()->slotTimer()->stop(webSocketChannel->slotTimerElement_);

		if (error) {
			Log(Debug, "WebSocketServer receive message content error; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}

		WebSocketMessage webSocketMessage;
		webSocketMessage.channelId_ = webSocketChannel->id_;
		webSocketMessage.message_ = "";

		std::istream is(&webSocketChannel->recvBuffer_);

		char maskingKey[4];
		is.read(maskingKey, 4);
		bytes_transfered -= 4;

		uint32_t pos = 0;
		while (bytes_transfered > 0) {
			char buffer[10000];
			uint32_t bufferLen = 10000;

			if (bytes_transfered < bufferLen) bufferLen = bytes_transfered;
			bytes_transfered -= bufferLen;

			is.read(buffer, bufferLen);

			// demasking
			for (uint32_t idx=0; idx<bufferLen; idx++) {
				buffer[idx] = buffer[idx] ^ maskingKey[pos%4];
				pos++;
			}

			webSocketMessage.message_.append(buffer, bufferLen);
		}

		if (receiveMessageCallback_) receiveMessageCallback_(webSocketMessage);
		receiveMessage(webSocketChannel);
	}

	void
	WebSocketServerBase::idleTimeoutWebSocketChannel(WebSocketChannel* webSocketChannel, const std::string& location)
	{
		Log(Debug, "WebSocketServer idle timeout; close channel")
			.parameter("Address", webSocketChannel->partner_.address().to_string())
			.parameter("Port", webSocketChannel->partner_.port())
			.parameter("Location", location)
			.parameter("ChannelId", webSocketChannel->channelId_);

		webSocketChannel->timeout_ = true;
		webSocketChannel->cancel();
	}

	// ------------------------------------------------------------------------
	// ------------------------------------------------------------------------
	//
	// handle send message
	//
	// ------------------------------------------------------------------------
	// ------------------------------------------------------------------------
	bool
	WebSocketServerBase::sendMessage(WebSocketMessage& webSocketMessage, WebSocketChannel* webSocketChannel)
	{
		std::ostream os(&webSocketChannel->sendBuffer_);

		char headerBytes[10];

		// set FIN and text frame
		headerBytes[0] = (char)0x81;

		// set length
		uint32_t headerLength;
		uint64_t length = webSocketMessage.message_.length();
		if (length <= 125) {
			headerLength = 2;
			headerBytes[1] = (uint8_t)length;
		}
		else if (length <= 0xFFFF) {
			headerLength = 4;
			headerBytes[1] = 126;

			char x;
			x = ((length>>(8*1))%256) & 0xFF;
			headerBytes[2] = x;
			x = (length%256>>(8*0)) & 0xFF;
			headerBytes[3] = x;
		}
		else {
			headerLength = 10;
			headerBytes[1] = 127;

			Log(Debug, "WebSocketServer do not support 8 byte length field; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return false;
		}
		os.write(headerBytes, headerLength);

		// set message
		os.write(webSocketMessage.message_.c_str(), length);

		// send message
		webSocketChannel->async_write(
			webSocketChannel->sendBuffer_,
			boost::bind(&WebSocketServerBase::handleWriteMessageComplete, this, boost::asio::placeholders::error, webSocketChannel)
		);

		return true;
	}

	void
	WebSocketServerBase::handleWriteMessageComplete(const boost::system::error_code& error, WebSocketChannel* webSocketChannel)
	{
		if (error) {
			Log(Debug, "WebSocketServer send response error; close channel")
				.parameter("Address", webSocketChannel->partner_.address().to_string())
				.parameter("Port", webSocketChannel->partner_.port())
				.parameter("ChannelId", webSocketChannel->channelId_);

			closeWebSocketChannel(webSocketChannel);
			return;
		}
	}

}