/*
 * Connecting to Mirac
 * Copyright 2024 matching
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>

#include <algorithm>
#include <cstddef>
#include <string>

#include "logoutput.hpp"

// 接続確立のタイムアウト(ms)
#define MIRAKC_CONNECT_TIMEOUT_MS 5000
// ヘッダ/APIボディ受信中の無通信タイムアウト(ms)。ストリーム受信中は無効にする
#define MIRAKC_IO_TIMEOUT_MS 10000
// レスポンスヘッダの最大サイズ
#define MIRAKC_HEADER_MAX_TOTAL ( 64 * 1024 )

/*
 * スレッドに関する注意:
 * ソケットfd(s)のclose(disconnect())は、受信スレッドが動いていない状態でのみ
 * 呼ぶこと(受信スレッドの終了後、または起動前)。受信スレッドの停止には
 * shutdown()を使う。受信スレッド自身がclose()すると、別スレッドのshutdown()と
 * 競合し、再利用された別のfdに対してshutdown()してしまう恐れがあるため。
 */
class MirakcConnectBase
{
protected:
	int s; // socket

	char buf[ 188 * 256 + 1 ];
	int bufsize;

	int connectFd( int fd, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms );
	void setIoTimeout( int ms );
	int sendRequest( const char *url, const char *requestHeader );
	int recvHeader( char *responceHeader, int *responceCode, int headermax );
	static ssize_t recvRetry( int fd, char *b, size_t n );

public:
	MirakcConnectBase() : s(-1), bufsize(0)
	{
	}
	virtual ~MirakcConnectBase()
	{
		if(s >= 0) {
			close(s);
			s = -1;
		}
	}

	virtual int connect() = 0;

	// *body はmalloc()で確保され、呼び出し側がfree()する。常にNUL終端される
	int sendGetRequest_WaitBody( const char *url, const char *requestHeader, char *responceHeader, int *responceCode, char **body, int *bodysize, int bodymax, int headermax );
	int sendGetRequest_WaitHeader( const char *url, const char *requestHeader, char *responceHeader, int *responceCode, int headermax );

	int recvBody( char *body, size_t size );

	void disconnect();
	void shutdown();
};

class MirakcConnectHttp : public MirakcConnectBase
{
private:
	std::string g_host;
	int g_port;
public:
	MirakcConnectHttp( const char *host, int port );
	int connect();
};

class MirakcConnectUnix : public MirakcConnectBase
{
private:
	struct sockaddr_un addr;
	bool path_too_long;

public:
	MirakcConnectUnix( const char *path );
	int connect();
};



/*************************************************/

int MirakcConnectBase::connectFd( int fd, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms )
{
	int flags = ::fcntl( fd, F_GETFL, 0 );
	if( flags < 0 ) return -1;
	if( ::fcntl( fd, F_SETFL, flags | O_NONBLOCK ) < 0 ) return -1;

	int ret = ::connect( fd, addr, addrlen );
	if( ret < 0 ) {
		if( errno != EINPROGRESS && errno != EINTR ) {
			return -1;
		}
		struct pollfd pfd;
		pfd.fd = fd;
		pfd.events = POLLOUT;
		pfd.revents = 0;
		do {
			ret = ::poll( &pfd, 1, timeout_ms );
		} while( ret < 0 && errno == EINTR );
		if( ret == 0 ) {
			errno = ETIMEDOUT;
			return -1;
		}
		if( ret < 0 ) {
			return -1;
		}
		int err = 0;
		socklen_t errlen = sizeof( err );
		if( ::getsockopt( fd, SOL_SOCKET, SO_ERROR, &err, &errlen ) < 0 ) {
			return -1;
		}
		if( err != 0 ) {
			errno = err;
			return -1;
		}
	}

	// ブロッキングモードに戻す
	if( ::fcntl( fd, F_SETFL, flags ) < 0 ) return -1;
	return 0;
}

void MirakcConnectBase::setIoTimeout( int ms )
{
	if( s < 0 ) return;
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = ( ms % 1000 ) * 1000;
	::setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) );
	::setsockopt( s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) );
}

ssize_t MirakcConnectBase::recvRetry( int fd, char *b, size_t n )
{
	ssize_t ret;
	do {
		ret = ::recv( fd, b, n, 0 );
	} while( ret < 0 && errno == EINTR );
	return ret;
}

/*************************************************/

MirakcConnectHttp::MirakcConnectHttp( const char *host, int port ) : g_host( host ? host : "" ), g_port( port )
{
}


int MirakcConnectHttp::connect()
{
	if( s >= 0 ) { ::close(s); s = -1; }
	bufsize = 0;

	char portstr[ 16 ];
	::snprintf( portstr, sizeof( portstr ), "%u", (unsigned int)( g_port & 0xffff ) );

	struct addrinfo hints;
	struct addrinfo *res = NULL;
	::memset( &hints, 0, sizeof( hints ) );
	hints.ai_family = AF_UNSPEC; // IPv4/IPv6, ホスト名(localhost等)も可
	hints.ai_socktype = SOCK_STREAM;

	int gai = ::getaddrinfo( g_host.c_str(), portstr, &hints, &res );
	if( gai != 0 || res == NULL ) {
		ERROR_OUTPUT( "getaddrinfo(%s) failed: %s", g_host.c_str(), ::gai_strerror( gai ) );
		errno = EHOSTUNREACH;
		return -1;
	}

	int last_errno = ECONNREFUSED;
	for( struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next ) {
		int fd = ::socket( ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol );
		if( fd < 0 ) {
			last_errno = errno;
			continue;
		}
		if( connectFd( fd, ai->ai_addr, ai->ai_addrlen, MIRAKC_CONNECT_TIMEOUT_MS ) == 0 ) {
			s = fd;
			::freeaddrinfo( res );
			return 0;
		}
		last_errno = errno;
		::close( fd );
	}
	::freeaddrinfo( res );
	errno = last_errno;
	return -1;
}

/*************************************************/


MirakcConnectUnix::MirakcConnectUnix( const char *path ) : path_too_long(false)
{
	::memset( &addr, 0, sizeof( addr ) );
	addr.sun_family = AF_UNIX;

	if( path != NULL && strlen( path ) <= sizeof( addr.sun_path ) - 1 ) {
		strcpy( addr.sun_path, path );
	}
	else {
		// 長すぎるパスを黙って空(=抽象ソケット名)にすると意図しない接続を試みるため、
		// 接続時にエラーとする
		path_too_long = true;
	}
}


int MirakcConnectUnix::connect()
{
	if( s >= 0 ) { ::close(s); s = -1; }
	bufsize = 0;
	if( path_too_long || addr.sun_path[0] == '\0' ) {
		ERROR_OUTPUT1( "invalid SERVER_SOCKPATH (empty or too long)" );
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = ::socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0 );
	if( fd < 0 ) return -1;
	if( connectFd( fd, (sockaddr *)&addr, sizeof( addr ), MIRAKC_CONNECT_TIMEOUT_MS ) < 0 ) {
		int e = errno;
		::close( fd );
		errno = e;
		return -1;
	}
	s = fd;
	return 0;
}


/*************************************************/

void MirakcConnectBase::shutdown()
{
	int fd = s;
	if( fd >= 0 ) {
		// SHUT_RDWRにすると、別スレッドでブロック中のrecv()をclose()せずに
		// 即座に0で復帰させられる。close()はfd番号の再利用レースがあるため避ける。
		::shutdown(fd, SHUT_RDWR);
	}
}

void MirakcConnectBase::disconnect()
{
	if( s >= 0 ) { ::close(s); s = -1; }
	bufsize = 0;
}

int MirakcConnectBase::sendRequest( const char *url, const char *requestHeader )
{
	char send_string[ 4096 ];
	int len = ::snprintf( send_string, sizeof(send_string), "GET %s HTTP/1.0\r\nHost: localhost\r\n%s\r\n\r\n", url, requestHeader );
	if( len < 0 || (size_t)len >= sizeof( send_string ) ) {
		ERROR_OUTPUT1( "request too long" );
		return -1;
	}

	int sent = 0;
	while( sent < len ) {
		// MSG_NOSIGNAL: 相手が切断済みでもSIGPIPEでホストアプリごと落ちないようにする
		ssize_t r = ::send( s, send_string + sent, len - sent, MSG_NOSIGNAL );
		if( r < 0 ) {
			if( errno == EINTR ) continue;
			return -errno - 1000;
		}
		sent += (int)r;
	}
	return 0;
}

// レスポンスヘッダを受信する。"\r\n\r\n"がrecvの境目をまたいでも検出できるよう、
// 受信データを蓄積して探す。1xx(暫定応答)は読み飛ばす。
// ヘッダ直後に届いていた本文は buf/bufsize に残す。
int MirakcConnectBase::recvHeader( char *responceHeader, int *responceCode, int headermax )
{
	std::string acc;
	*responceCode = 0;
	if( headermax > 0 ) responceHeader[0] = '\0';

	for(;;) {
		for(;;) {
			size_t pos = acc.find( "\r\n\r\n" );
			if( pos == std::string::npos ) break;

			size_t sp = acc.find( ' ' );
			if( acc.compare( 0, 5, "HTTP/" ) != 0 || sp == std::string::npos || sp > pos ) {
				ERROR_OUTPUT1( "malformed status line" );
				return -1;
			}
			int code = ::atoi( acc.c_str() + sp + 1 );
			size_t hlen = pos + 4;

			if( code >= 100 && code <= 199 ) {
				DEBUG_OUTPUT( "skip 1xx interim response (%d)", code );
				acc.erase( 0, hlen );
				continue;
			}

			*responceCode = code;
			if( headermax > 0 ) {
				size_t c = std::min( hlen, (size_t)headermax - 1 );
				if( c < hlen ) {
					ERROR_OUTPUT( "response header exceeds buffer (max:%d), truncating", headermax );
				}
				::memcpy( responceHeader, acc.data(), c );
				responceHeader[ c ] = '\0';
			}

			size_t rest = acc.size() - hlen;
			if( rest > sizeof( buf ) - 1 ) {
				ERROR_OUTPUT1( "unexpected data after header is too large" );
				return -1;
			}
			if( rest > 0 ) {
				::memcpy( buf, acc.data() + hlen, rest );
			}
			bufsize = (int)rest;
			return 0;
		}

		if( acc.size() > MIRAKC_HEADER_MAX_TOTAL ) {
			ERROR_OUTPUT1( "response header too large" );
			return -1;
		}

		ssize_t n = recvRetry( s, buf, sizeof( buf ) - 1 );
		if( n < 0 ) {
			int e = errno;
			ERROR_OUTPUT( "recv error while reading header (%d)", e );
			return -1;
		}
		if( n == 0 ) {
			// ヘッダが完結する前に切断された
			ERROR_OUTPUT1( "connection closed before response header completed" );
			return -3;
		}
		acc.append( buf, (size_t)n );
	}
}

int MirakcConnectBase::sendGetRequest_WaitBody( const char *url, const char *requestHeader, char *responceHeader, int *responceCode, char **body, int *bodysize, int bodymax, int headermax )
{
	*responceCode = 0;
	*body = NULL;
	*bodysize = 0;

	int ret = connect();
	if( ret < 0 ) {
		return -errno - 2000;
	}
	setIoTimeout( MIRAKC_IO_TIMEOUT_MS );

	ret = sendRequest( url, requestHeader );
	if( ret < 0 ) {
		disconnect();
		return ret;
	}

	ret = recvHeader( responceHeader, responceCode, headermax );
	if( ret != 0 ) {
		disconnect();
		return ret;
	}

	// 本文は必要なだけ伸長して受け取る(上限はbodymax)
	std::string data( buf, (size_t)bufsize );
	bufsize = 0;
	for(;;) {
		if( bodymax > 0 && data.size() > (size_t)bodymax ) {
			ERROR_OUTPUT( "response body exceeds limit (max:%d)", bodymax );
			disconnect();
			return -4;
		}
		ssize_t n = recvRetry( s, buf, sizeof( buf ) - 1 );
		if( n < 0 ) {
			ERROR_OUTPUT( "recv error while reading body (%d)", errno );
			disconnect();
			return -5;
		}
		if( n == 0 ) break;
		data.append( buf, (size_t)n );
	}
	disconnect();

	char *out = (char *)::malloc( data.size() + 1 );
	if( out == NULL ) {
		return -6;
	}
	::memcpy( out, data.data(), data.size() );
	out[ data.size() ] = '\0';
	*body = out;
	*bodysize = (int)data.size();
	return 0;
}


int MirakcConnectBase::sendGetRequest_WaitHeader( const char *url, const char *requestHeader, char *responceHeader, int *responceCode, int headermax )
{
	*responceCode = 0;

	int ret = connect();
	if( ret < 0 ) {
		return -errno - 2000;
	}
	setIoTimeout( MIRAKC_IO_TIMEOUT_MS );

	ret = sendRequest( url, requestHeader );
	if( ret < 0 ) {
		return ret;
	}

	ret = recvHeader( responceHeader, responceCode, headermax );
	if( ret != 0 ) {
		return ret;
	}

	// ヘッダ受信後はストリームを受け続けるので、無通信タイムアウトは解除する
	setIoTimeout( 0 );
	return 0;
}

int MirakcConnectBase::recvBody( char *body, size_t size )
{
	if( bufsize > 0 ){
		int len = (int)std::min( (size_t)bufsize, size );
		::memcpy( body, buf, len );
		if( bufsize - len > 0 ) ::memmove( buf, buf + len, bufsize - len );
		bufsize -= len;
		return len;
	}

	// 既に切断済み(s == -1)の場合は recv() を呼ばない。
	if( s < 0 ) {
		return 0; // 切断済みとして扱う
	}

	ssize_t ret = recvRetry( s, body, size );

	if( ret < 0 ) {
		return -errno - 10000;
	}

	return (int)ret;
}
