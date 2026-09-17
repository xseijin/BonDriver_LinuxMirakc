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

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>

#include <algorithm>
#include <cstddef>

#include "logoutput.hpp"

class MirakcConnectBase
{
protected:
	int s; // socket

	char buf[ 188 * 256 + 1 ];
	int bufsize;

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

	int sendGetRequest_WaitBody( char *url, char *requestHeader, char *responceHeader, int *responceCode, char *body, int *bodysize, int bodymax, int headermax );
	int sendGetRequest_WaitHeader( char *url, char *requestHeader, char *responceHeader, int *responceCode, int headermax );

	int recvBody( char *body, size_t size );

	void disconnect();
	void shutdown();
};

class MirakcConnectHttp : public MirakcConnectBase
{
private:
	struct sockaddr_in s_addr;
	char g_host[256];
	short g_port;
public:
	MirakcConnectHttp( char *host, short port );
	int connect();
};

class MirakcConnectUnix : public MirakcConnectBase
{
private:
	struct sockaddr_un addr;

public:
	MirakcConnectUnix( char *path );
	int connect();
};



/*************************************************/

MirakcConnectHttp::MirakcConnectHttp( char *host, short port ) : g_port(port)
{
	strncpy( g_host, host, sizeof(g_host) - 1 );
	g_host[ sizeof(g_host) - 1 ] = '\0';
	s_addr.sin_addr.s_addr = inet_addr( host );
	s_addr.sin_port        = htons( port );
	s_addr.sin_family      = AF_INET;
}


int MirakcConnectHttp::connect()
{
	if( s >= 0 ) { ::close(s); s = -1; }
	bufsize = 0;
	s = socket(AF_INET, SOCK_STREAM, 0);
	if( s < 0 ) return -1;
	int ret = ::connect( s, (sockaddr *)&s_addr, sizeof( s_addr ) );
	if( ret < 0 ) { ::close(s); s = -1; }
	return ret;
}

/*************************************************/


MirakcConnectUnix::MirakcConnectUnix( char *path )
{
	addr.sun_family = AF_UNIX;
	
	if( strlen( path ) <= sizeof( addr.sun_path ) - 1 ) {
		strcpy( addr.sun_path, path );
	}
	else {
		addr.sun_path[0] = 0;
	}
}


int MirakcConnectUnix::connect()
{
	if( s >= 0 ) { ::close(s); s = -1; }
	bufsize = 0;
	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if( s < 0 ) return -1;
	int ret = ::connect( s, (sockaddr *)&addr, sizeof( addr ) );
	if( ret < 0 ) { ::close(s); s = -1; }
	return ret;
}


/*************************************************/

void MirakcConnectBase::shutdown()
{
	if( s >= 0 ) {
		// SHUT_WR（書き込み側）のみでは、mirakc側がまだTSデータを送信し続けている限り、
		// 別スレッド(RecvThread)がブロックしている recv() は起きない。
		// 結果として waitForRecvThreadFinish() の pthread_join() が永久に戻らず、
		// デッドロック（ハング）する。
		// SHUT_RDWR（読み込み側も含める）にすると、同じfdでブロック中のrecv()を
		// close()せずに即座に0で復帰させられるため、これを使う
		// （close()での強制解放はfd番号が別スレッドの新しい接続に再利用される
		//   レースの危険があるため避ける）。
		::shutdown(s, SHUT_RDWR);
	}

}

void MirakcConnectBase::disconnect()
{
	if( s >= 0 ) { ::close(s); s = -1; }
	bufsize = 0;
}

int MirakcConnectBase::sendGetRequest_WaitBody( char *url, char *requestHeader, char *responceHeader, int *responceCode, char *body, int *bodysize, int bodymax, int headermax )
{
	int ret;
	int state = 0;
	char *p_startbody = body;
	// bodymax/headermax はターミネータ用の1バイトを含めた「呼び出し側が確保したバッファ全体のサイズ」
	// これを超えてコピーしないようにする（呼び出し側バッファのオーバーフロー対策）
	char *p_bodylimit = p_startbody + ( bodymax > 0 ? bodymax - 1 : 0 ); // ヌル終端分を1バイト確保
	char *p_headerstart = responceHeader;
	char *p_headerlimit = p_headerstart + ( headermax > 0 ? headermax - 1 : 0 );

	ret = connect();
	if( ret < 0 ) {
		return -errno - 2000;
	}

	char send_string[ 4096 ];
	::snprintf( send_string, sizeof(send_string), "GET %s HTTP/1.0\r\nHost: localhost\r\n%s\r\n\r\n", url, requestHeader );

	ret = ::send( s, send_string, strlen( send_string ), 0 );
	if( ret < 0 ) {
		
		return ret - 1000;
	}

	for(;;) {
		ret = ::recv( s, buf, sizeof(buf) - 1, 0 );
		if( ret < 0 ){
			return -1;
		}
		if( ret == 0 ){
			*body = 0;
			*bodysize = body - p_startbody;

			disconnect();
			return 0;
		}
		buf[ ret ] = 0;

		// 1xx暫定応答の読み飛ばし、およびヘッダー終端直後に届いた本文データを
		// recv()を挟まずに同じチャンク内で処理し切るための内側ループ
		for(;;) {
			if( state == 0 ) { // state reading
				char *p;
				p = ::strchr( buf, ' ' );
				if( p == NULL ) {
					// ステータス行が不正（スペースが見つからない）
					ERROR_OUTPUT1( "sendGetRequest_WaitBody: malformed status line" );
					disconnect();
					return -1;
				}
				*responceCode = ::atoi( p + 1 );
				state = 1;
			}
			if( state == 1 ){ // break through
				char *p;
				
				p = ::strstr( buf, "\r\n\r\n" );
				if( p != NULL ) {
					int hlen = (int)(p + 4 - buf);
					int rest = ret - hlen;

					if( *responceCode >= 100 && *responceCode <= 199 ) {
						// 1xx (例: 100 Continue) は暫定応答。破棄して次の本応答を待つ
						DEBUG_OUTPUT( "sendGetRequest_WaitBody: skip 1xx interim response (%d)", *responceCode );
						responceHeader = p_headerstart; // ヘッダー蓄積位置をリセット
						state = 0;
						if( rest > 0 ) {
							// 続きのデータが同じrecv()チャンクに含まれている可能性があるため、
							// recv()を待たずにその場で再解析する
							::memmove( buf, buf + hlen, rest );
							ret = rest;
							buf[ ret ] = 0;
							continue;
						}
						else {
							// 続きはまだ届いていないので、次のrecv()を待つ
							break;
						}
					}

					// 通常（1xxでない）の最終応答としてヘッダーを確定する
					{
						ptrdiff_t hremain = p_headerlimit - responceHeader;
						if( hremain < 0 ) hremain = 0;
						ptrdiff_t hcopy = hlen;
						if( hcopy > hremain ) {
							ERROR_OUTPUT( "sendGetRequest_WaitBody: response header exceeds buffer (max:%d), truncating", headermax );
							hcopy = hremain;
						}
						if( hcopy > 0 ) {
							::memcpy( responceHeader, buf, hcopy );
							responceHeader += hcopy;
						}
					}
					*responceHeader = 0;

					state = 2;

					if( rest > 0 ) {
						// ヘッダーの直後に届いた本文データを取りこぼさないよう、
						// このまま下のstate==2処理に流し込む
						::memmove( buf, buf + hlen, rest );
						ret = rest;
					}
					else {
						ret = 0;
					}
				}
				else {
					ptrdiff_t hremain = p_headerlimit - responceHeader;
					if( hremain < 0 ) hremain = 0;
					ptrdiff_t hcopy = ret;
					if( hcopy > hremain ) {
						ERROR_OUTPUT( "sendGetRequest_WaitBody: response header exceeds buffer (max:%d), truncating", headermax );
						hcopy = hremain;
					}
					if( hcopy > 0 ) {
						::memcpy( responceHeader, buf, hcopy );
						responceHeader += hcopy;
					}
				}
			}

			if( state == 2 ) {
				if( ret > 0 ) {
					// 呼び出し側バッファの残り容量を超える分は破棄する（オーバーフロー防止）
					ptrdiff_t remain = p_bodylimit - body;
					if( remain < 0 ) remain = 0;
					int copylen = ret;
					if( (ptrdiff_t)copylen > remain ) {
						ERROR_OUTPUT( "sendGetRequest_WaitBody: response body exceeds buffer (max:%d), truncating", bodymax );
						copylen = (int)remain;
					}
					if( copylen > 0 ) {
						::memcpy( body, buf, copylen );
						body += copylen;
					}
				}
			}

			break; // 内側ループを抜けて外側のrecv()へ
		}
	}
	// not reached
}


int MirakcConnectBase::sendGetRequest_WaitHeader( char *url, char *requestHeader, char *responceHeader, int *responceCode, int headermax )
{
	int ret;
	int state = 0;
	char *p_headerstart = responceHeader;
	// headermax はターミネータ用の1バイトを含めた「呼び出し側が確保したバッファ全体のサイズ」
	char *p_headerlimit = p_headerstart + ( headermax > 0 ? headermax - 1 : 0 );

	ret = connect();
	if( ret < 0 ) {
		return -errno - 2000;
	}
	char send_string[ 4096 ];
	::snprintf( send_string, sizeof(send_string), "GET %s HTTP/1.0\r\nHost: localhost\r\n%s\r\n\r\n", url, requestHeader );

	ret = ::send( s, send_string, strlen( send_string ), 0 );
	if( ret < 0 ) {
		return -errno - 1000;
	}
	
	for(;;) {
		ret = ::recv( s, buf, sizeof(buf) - 1, 0 );
		if( ret < 0 ){
			return -1;
		}
		if( ret == 0 ){
			close(s);
			s=-1;
			return 0;
		}
		buf[ ret ] = 0;

		// 1xx暫定応答の読み飛ばし用の内側ループ（recv()を挟まずに同一チャンクを再解析する）
		for(;;) {
			if( state == 0 ) { // state reading
				char *p;
				p = ::strchr( buf, ' ' );
				if( p == NULL ) {
					// ステータス行が不正（スペースが見つからない）
					ERROR_OUTPUT1( "sendGetRequest_WaitHeader: malformed status line" );
					close(s);
					s = -1;
					return -1;
				}
				*responceCode = ::atoi( p + 1 );
				state = 1;
			}
			if( state == 1 ){ // break through
				char *p;
				
				p = ::strstr( buf, "\r\n\r\n" );
				if( p != NULL ) {
					int hlen = (int)(p + 4 - buf);
					int body_in_chunk = ret - hlen;

					if( *responceCode >= 100 && *responceCode <= 199 ) {
						// 1xx (例: 100 Continue) は暫定応答。破棄して次の本応答を待つ
						DEBUG_OUTPUT( "sendGetRequest_WaitHeader: skip 1xx interim response (%d)", *responceCode );
						responceHeader = p_headerstart; // ヘッダー蓄積位置をリセット
						state = 0;
						if( body_in_chunk > 0 ) {
							// 続きのデータが同じrecv()チャンクに含まれている可能性があるため、
							// recv()を待たずにその場で再解析する
							::memmove( buf, buf + hlen, body_in_chunk );
							ret = body_in_chunk;
							buf[ ret ] = 0;
							continue;
						}
						else {
							// 続きはまだ届いていないので、次のrecv()を待つ
							break;
						}
					}

					// ヘッダーバッファの残り容量を超える分は破棄する（オーバーフロー防止）
					ptrdiff_t hremain = p_headerlimit - responceHeader;
					if( hremain < 0 ) hremain = 0;
					ptrdiff_t hcopy = hlen;
					if( hcopy > hremain ) {
						ERROR_OUTPUT( "sendGetRequest_WaitHeader: response header exceeds buffer (max:%d), truncating", headermax );
						hcopy = hremain;
					}
					if( hcopy > 0 ) {
						::memcpy( responceHeader, buf, hcopy );
						responceHeader += hcopy;
					}
					*responceHeader = 0;
					if( body_in_chunk > 0 ) {
						::memmove( buf, buf + hlen, body_in_chunk );
						bufsize = body_in_chunk;
					} else {
						bufsize = 0;
					}
					state = 2;
				}
				else {
					ptrdiff_t hremain = p_headerlimit - responceHeader;
					if( hremain < 0 ) hremain = 0;
					ptrdiff_t hcopy = ret;
					if( hcopy > hremain ) {
						ERROR_OUTPUT( "sendGetRequest_WaitHeader: response header exceeds buffer (max:%d), truncating", headermax );
						hcopy = hremain;
					}
					if( hcopy > 0 ) {
						::memcpy( responceHeader, buf, hcopy );
						responceHeader += hcopy;
					}
				}
			}
			if( state == 2 ) {
				return 0; // !
			}
			break; // 内側ループを抜けて外側のrecv()へ
		}
	}
	// not reached
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

	// 既に切断済み（s == -1）の場合は recv() を呼ばない。
	// close()済みのfd番号は他スレッドの新しい接続に再利用され得るため、
	// 無効な状態のまま recv() を呼び続けるのは危険（状態不整合）。
	if( s < 0 ) {
		return 0; // 切断済みとして扱う（呼び出し元のRecvThreadはこれをdisconnect扱いする）
	}

	int ret;
	
	ret = ::recv( s, body, size, 0 );

	if( ret < 0 ) {
		return -errno - 10000;
	}

	return ret;
}


