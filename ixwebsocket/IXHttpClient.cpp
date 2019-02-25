/*
 *  IXHttpClient.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2019 Machine Zone, Inc. All rights reserved.
 */

#include "IXHttpClient.h"
#include "IXUrlParser.h"
#include "IXWebSocketHttpHeaders.h"

#if defined(__APPLE__) or defined(__linux__)
# ifdef __APPLE__
#  include <ixwebsocket/IXSocketAppleSSL.h>
# else
#  include <ixwebsocket/IXSocketOpenSSL.h>
# endif
#endif

#include <iostream>
#include <sstream>
#include <iomanip>
#include <vector>

namespace ix
{
    HttpClient::HttpClient()
    {

    }

    HttpClient::~HttpClient()
    {

    }

    HttpResponse HttpClient::request(
        const std::string& url,
        const std::string& verb,
        const HttpParameters& httpParameters,
        bool verbose)
    {
        int code = 0;
        WebSocketHttpHeaders headers;
        std::string payload;

        std::string protocol, host, path, query;
        int port;

        if (!parseUrl(url, protocol, host, path, query, port))
        {
            code = 0; // 0 ?
            std::string errorMsg("Cannot parse url");
            return std::make_tuple(code, headers, payload, errorMsg);
        }

        if (protocol == "http")
        {
            _socket = std::make_shared<Socket>();
        }
        else if (protocol == "https")
        {
# ifdef __APPLE__
            _socket = std::make_shared<SocketAppleSSL>();
# else
            _socket = std::make_shared<SocketOpenSSL>();
# endif
        }
        else
        {
            code = 0; // 0 ?
            std::string errorMsg("Bad protocol");
            return std::make_tuple(code, headers, payload, errorMsg);
        }

        std::string body;
        if (verb == "POST")
        {
            std::stringstream ss;
            size_t count = httpParameters.size();
            size_t i = 0;

            for (auto&& it : httpParameters)
            {
                ss << urlEncode(it.first)
                   << "="
                   << urlEncode(it.second);

                if (i++ < (count-1))
                {
                   ss << "&";
                }
            }
            body = ss.str();
        }

        std::stringstream ss;
        ss << verb << " " << path << " HTTP/1.1\r\n";
        ss << "Host: " << host << "\r\n";
        ss << "User-Agent: ixwebsocket/1.0.0" << "\r\n";
        ss << "Accept: */*" << "\r\n";
        if (verb == "POST")
        {
            ss << "Content-Length: " << body.size() << "\r\n";
            ss << "Content-Type: application/x-www-form-urlencoded" << "\r\n";
            ss << "\r\n";
            ss << body;
        }
        else
        {
            ss << "\r\n";
        }

        std::string request(ss.str());

        int timeoutSecs = 10;

        std::string errMsg;
        static std::atomic<bool> requestInitCancellation(false);
        auto isCancellationRequested =
            makeCancellationRequestWithTimeout(timeoutSecs, requestInitCancellation);

        bool success = _socket->connect(host, port, errMsg, isCancellationRequested);
        if (!success)
        {
            code = 0; // 0 ?
            std::string errorMsg("Cannot connect to url");
            return std::make_tuple(code, headers, payload, errorMsg);
        }

        if (verbose)
        {
            std::cout << "Sending " << verb << " request "
                      << "to " << host << ":" << port << std::endl
                      << "request size: " << request.size() << " bytes"
                      << "=============" << std::endl
                      << request
                      << "=============" << std::endl
                      << std::endl;
        }

        if (!_socket->writeBytes(request, isCancellationRequested))
        {
            code = 0; // 0 ?
            std::string errorMsg("Cannot send request");
            return std::make_tuple(code, headers, payload, errorMsg);
        }

        auto lineResult = _socket->readLine(isCancellationRequested);
        auto lineValid = lineResult.first;
        auto line = lineResult.second;

        if (!lineValid)
        {
            code = 0; // 0 ?
            std::string errorMsg("Cannot retrieve status line");
            return std::make_tuple(code, headers, payload, errorMsg);
        }

        if (verbose)
        {
            std::cout << "first line: " << line << std::endl;
        }

        code = -1;
        if (sscanf(line.c_str(), "HTTP/1.1 %d", &code) != 1)
        {
            code = 0; // 0 ?
            std::string errorMsg("Cannot parse response code from status line");
            return std::make_tuple(code, headers, payload, errorMsg);
        }

        auto result = parseHttpHeaders(_socket, isCancellationRequested);
        auto headersValid = result.first;
        headers = result.second;

        if (!headersValid)
        {
            code = 0; // 0 ?
            std::string errorMsg("Cannot parse http headers");
            return std::make_tuple(code, headers, payload, errorMsg);
        }

        // Parse response:
        // http://bryce-thomas.blogspot.com/2012/01/technical-parsing-http-to-extract.html
        if (headers.find("content-length") == headers.end())
        {
            code = 0; // 0 ?
            std::string errorMsg("No content length header");
            return std::make_tuple(code, headers, payload, errorMsg);
        }

        ssize_t contentLength = -1;
        ss.str("");
        ss << headers["content-length"];
        ss >> contentLength;

        payload.reserve(contentLength);

        // very inefficient way to read bytes, but it works...
        for (int i = 0; i < contentLength; ++i)
        {
            char c;
            if (!_socket->readByte(&c, isCancellationRequested))
            {
                ss.str("");
                ss << "Cannot read byte";
                return std::make_tuple(-1, headers, payload, ss.str());
            }

            payload += c;
        }

        return std::make_tuple(code, headers, payload, "");
    }

    HttpResponse HttpClient::get(
        const std::string& url,
        bool verbose)
    {
        return request(url, "GET", HttpParameters(), verbose);
    }

    HttpResponse HttpClient::post(
        const std::string& url,
        const HttpParameters& httpParameters,
        bool verbose)
    {
        return request(url, "POST", httpParameters, verbose);
    }

    std::string HttpClient::urlEncode(const std::string& value)
    {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (std::string::const_iterator i = value.begin(), n = value.end();
             i != n; ++i)
        {
            std::string::value_type c = (*i);

            // Keep alphanumeric and other accepted characters intact
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                escaped << c;
                continue;
            }

            // Any other characters are percent-encoded
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int((unsigned char) c);
            escaped << std::nouppercase;
        }

        return escaped.str();
    }
}

#if 0
        std::vector<uint8_t> rxbuf;

        while (true)
        {
            int N = (int) _rxbuf.size();

            _rxbuf.resize(N + 1500);
            ssize_t ret = _socket->recv((char*)&_rxbuf[0] + N, 1500);

            if (ret < 0 && (_socket->getErrno() == EWOULDBLOCK ||
                            _socket->getErrno() == EAGAIN)) {
                _rxbuf.resize(N);
                break;
            }
            else if (ret <= 0)
            {
                _rxbuf.resize(N);

                _socket->close();
                setReadyState(CLOSED);
                break;
            }
            else
            {
                _rxbuf.resize(N + ret);
            }
        }
        ssize_t ret = _socket->recv((char*)&rxbuf[0], contentLength);
        payload = std::string(rxbuf.begin(), rxbuf.end());
        std::cerr << "socket->recv: " << ret << std::endl;

        if (ret != contentLength)
        {
            ss.str("");
            ss << "Cannot retrieve all bytes"
               << " want: " << contentLength
               << ", got: " << ret;
            std::cerr << "adscasdcadcasdc" << std::endl;
            std::cerr << ss.str() << std::endl;

            return std::make_tuple(-1, headers, payload, ss.str());
        }
#endif
