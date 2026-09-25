//   originaled by tkmsst
//   modified by matching

#include "BonDriver_LinuxMirakc.hpp"
#include "config.hpp"
#include "logoutput.hpp"

#include <string.h>
#include <dlfcn.h>
#include <exception>
#include <vector>

// g_TunerName の実体(宣言は BonDriver_LinuxMirakc.hpp / logoutput.hpp)
char g_TunerName[128];

static void Init_set_default_value(void);

static int Init()
{

	int ret;
	Dl_info info;
	char ini_filename[ MAX_PATH ];

	ret = dladdr( (void *)Init, &info );
	if( ret == 0 ) {
		ERROR_OUTPUT("dladdr %d", ret);
		return -1;
	}

	int written = snprintf( ini_filename, sizeof(ini_filename), "%s.ini", info.dli_fname );
	if( written < 0 || (size_t)written >= sizeof(ini_filename) ) {
		// パスが長すぎてini_filenameバッファに収まらない。iniが読めないだけなので
		// チューナー生成自体は失敗させず、config.Load()を失敗させてデフォルト値に
		// フォールバックさせる
		ERROR_OUTPUT1("module path too long to build ini filename. load default value");
		ini_filename[0] = '\0';
	}

	// チューナー名は .so を除いたファイル名とする（お尻は無条件に３文字削る）
	char *p;
	const char *base_name;
	size_t full_len, name_len;

	p = strrchr( (char *)info.dli_fname, '/' );
	base_name = ( p == NULL ) ? info.dli_fname : p + 1;

	full_len = strlen( base_name );
	// ファイル名が3文字以下の場合に size_t の引き算で桁あふれしないようにガードする
	name_len = ( full_len > 3 ) ? ( full_len - 3 ) : 0;
	// g_TunerName のバッファ（終端の1バイトを除く）を超えないようにクランプする
	if( name_len > sizeof( g_TunerName ) - 1 ) {
		name_len = sizeof( g_TunerName ) - 1;
	}
	::memcpy( g_TunerName, base_name, name_len );
	g_TunerName[ name_len ] = '\0';
	
	// load ini file
	Config config;
	Config::Section sec_global;

	if( config.Load( ini_filename ) == false ) {
		ERROR_OUTPUT("ini file(%s) not found. load default value", ini_filename);
		Init_set_default_value();
		return 0;
	}

	if( config.Exists( "GLOBAL" ) == false ) {
		ERROR_OUTPUT1("in ini file, GLOBAL section not found. load default value");
		Init_set_default_value();
		return 0;
	}

	sec_global = config.Get( "GLOBAL" );

	strncpy( g_ServerHost, sec_global.Get("SERVER_HOST", "127.0.0.1" ).c_str(), sizeof( g_ServerHost ) - 1 );
	g_ServerHost[ sizeof( g_ServerHost ) - 1 ] = '\0';
	g_ServerPort = sec_global.Get("SERVER_PORT", 40772 );
	g_DecodeB25 = sec_global.Get("DECODE_B25", 0 );
	g_Priority = sec_global.Get("PRIORITY", 1 );
	g_Service_Split = sec_global.Get("SERVICE_SPLIT", 0 );

	strncpy( g_ServerSockpath, sec_global.Get("SERVER_SOCKPATH", "").c_str(), sizeof( g_ServerSockpath ) - 1 );
	g_ServerSockpath[ sizeof( g_ServerSockpath ) - 1 ] = '\0';
	strncpy( g_ServerType, sec_global.Get("SERVER_TYPE", "http").c_str(), sizeof( g_ServerType ) - 1 );
	g_ServerType[ sizeof( g_ServerType ) - 1 ] = '\0';

	return 0;
}

static void Init_set_default_value(void)
{
	strcpy( g_ServerHost, "127.0.0.1" );
	g_ServerPort = 40772;
	g_DecodeB25 = 0;
	g_Priority = 1;
	g_Service_Split = 0;

	g_ServerSockpath[0] = '\0';
	strcpy( g_ServerType, "http" );
}


//////////////////////////////////////////////////////////////////////
// インスタンス生成メソッド
//////////////////////////////////////////////////////////////////////

extern "C" IBonDriver * CreateBonDriver()
{
	try {
		if( CBonTuner::m_pThis != NULL ) {
			// 既存インスタンスを共有して返す。Release()の二重deleteを避けるため参照カウントを増やす
			CBonTuner::m_pThis->m_refCount++;
			return CBonTuner::m_pThis;
		}
		
		int ret;
		
		ret = Init();
		if( ret < 0 ) {
			return NULL;
		}

		DEBUG_OUTPUT("SERVER_HOST:%s", g_ServerHost);
		DEBUG_OUTPUT("SERVER_PORT:%d", g_ServerPort);
		DEBUG_OUTPUT("SERVER_SOCKPATH:%s", g_ServerSockpath);

		DEBUG_OUTPUT("SERVER_TYPE:%s", g_ServerType);

		DEBUG_OUTPUT("DECODE_B25:%d", g_DecodeB25);
		DEBUG_OUTPUT("PRIORITY:%d", g_Priority);
		DEBUG_OUTPUT("SERVICE_SPLIT:%d", g_Service_Split);

		return (IBonDriver *) new CBonTuner;
	}
	catch (const std::exception& e) {
		// extern "C" 境界を越えて例外を伝播させると未定義動作になり、
		// ホストアプリ全体をクラッシュさせかねない（Init()内のstd::string/
		// std::unordered_map操作や new でのstd::bad_alloc等が経路になりうる）。
		ERROR_OUTPUT( "CreateBonDriver: exception (%s)", e.what() );
		return NULL;
	}
	catch (...) {
		ERROR_OUTPUT1( "CreateBonDriver: unknown exception" );
		return NULL;
	}
}

// 静的メンバ初期化
CBonTuner * CBonTuner::m_pThis = NULL;

CBonTuner::CBonTuner()
{
	DEBUG_CALL("");

	m_refCount = 1;
	m_dwCurSpace = 0xffffffff;
	m_dwCurChannel = 0xffffffff;

	m_hRecvThread = 0;
	m_bRecvThreadValid = false;

	conn = NULL;
	m_pGrabTsData = NULL;

	// GrabTsDataインスタンス作成
	m_pGrabTsData = new GrabTsData();

	// 例外で構築に失敗した場合にm_pThisがぶら下がらないよう、最後に設定する
	m_pThis = this;
}

CBonTuner::~CBonTuner()
{
	DEBUG_CALL("");

	// 開かれてる場合は閉じる
	CloseTuner();

	// GrabTsDataインスタンス開放
	if (m_pGrabTsData) {
		delete m_pGrabTsData;
	}

	m_pThis = NULL;
}

const BOOL CBonTuner::OpenTuner()
{
	DEBUG_CALL("");

	DEBUG_OUTPUT1("Start");

	// 既に開かれている場合（再Open）に備えて、まず確実に閉じてから始める。
	// これをしないと、古い conn オブジェクトや g_pType の確保領域が
	// 解放されずに上書きされ、リークの原因になる。
	CloseTuner();

	while (1) {
		conn = NULL;
		
		if( strcasecmp( g_ServerType, "http" ) == 0 ) {
			conn = new MirakcConnectHttp( g_ServerHost, g_ServerPort );
		}
		else if( strcasecmp( g_ServerType, "unix" ) == 0 ) {
			conn = new MirakcConnectUnix( g_ServerSockpath );
		}
		
		if( conn == NULL ) {
			ERROR_OUTPUT( "%s: Server Type Invalid (%s)", g_TunerName, g_ServerType );
			break;
		}
		
		if( conn->connect() < 0 ) {
			ERROR_OUTPUT( "%s: Connect error (%s)(%s)(%d)(%s)", g_TunerName, g_ServerType, g_ServerHost, g_ServerPort, g_ServerSockpath );
			break;
		}

		// todo error handling

		conn->disconnect(); // 要求時に接続するので切断

		//Initialize channel
		if (!InitChannel()) {
			break;
		}

		DEBUG_OUTPUT1("End(success)");

		return TRUE;
	}

	CloseTuner();

	DEBUG_OUTPUT1("End(fail)");

	return FALSE;
}

void CBonTuner::CloseTuner()
{
	DEBUG_CALL("");
	DEBUG_OUTPUT1("Start");

	// チャンネル初期化
	m_dwCurSpace = 0xffffffff;
	m_dwCurChannel = 0xffffffff;

	// スレッド終了
	waitForRecvThreadFinish();

	// チューニング空間解放
	for (int i = 0; i <= g_Max_Type; i++) {
		if (g_pType[i]) {
			free(g_pType[i]);
			g_pType[i] = NULL; // 解放後にダングリングポインタとして残さない
		}
	}
	g_Max_Type = -1;
	
	if(conn) {
		conn->disconnect();
		delete conn;
		conn = NULL;
	}
	DEBUG_OUTPUT1("End");
}

const DWORD CBonTuner::WaitTsStream(const DWORD dwTimeOut)
{
	DEBUG_CALL("");
	DEBUG_OUTPUT("Start(%d)", dwTimeOut);

	DWORD ret;

	ret = m_pGrabTsData->Wait_TsStream( dwTimeOut );

	DEBUG_OUTPUT("End(%d)", ret);
	return ret;
}

const DWORD CBonTuner::GetReadyCount()
{
	DEBUG_CALL("");
	DEBUG_OUTPUT1("Start");

	DWORD dwCount = 0;
	if (m_pGrabTsData) {
		m_pGrabTsData->get_ReadyCount(&dwCount);
	}

	DEBUG_OUTPUT("End(%d)", dwCount);
	return dwCount;
}

const BOOL CBonTuner::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	DEBUG_CALL("");

	if (pDst == NULL || pdwSize == NULL) {
		// pDst/pdwSizeがNULLで呼ばれた場合のヌルポインタ参照を防止
		return FALSE;
	}

	BYTE *pSrc = NULL;

	// TSデータをバッファから取り出す
	if (GetTsStream(&pSrc, pdwSize, pdwRemain)) {
		if (*pdwSize) {
			::memcpy( pDst, pSrc, *pdwSize );
		}

		return TRUE;
	}

	return FALSE;
}

const BOOL CBonTuner::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	DEBUG_CALL("");

	if (!m_pGrabTsData || m_dwCurChannel == 0xffffffff) {
		return FALSE;
	}

	BOOL ret;

	ret = m_pGrabTsData->get_TsStream(ppDst, pdwSize, pdwRemain);

	return ret;
}

void CBonTuner::PurgeTsStream()
{
	DEBUG_CALL("");

	if (m_pGrabTsData) {
		m_pGrabTsData->purge_TsStream();
	}
}

void CBonTuner::Release()
{
	DEBUG_CALL("");
	DEBUG_OUTPUT1("Called");

	// CreateBonDriver()が同じインスタンスを複数回返している場合があるため、
	// 最後の参照が解放されたときだけ実際に開放する
	if (--m_refCount > 0) {
		return;
	}

	// インスタンス開放
	delete this;
}

LPCTSTR CBonTuner::GetTunerName(void)
{
	DEBUG_CALL("");
	DEBUG_OUTPUT1("Called");

	// チューナ名を返す
	// スレッド間でバッファを共有すると、複数スレッドから同時に呼ばれた場合に
	// 内容が競合・破損する可能性があるため、スレッドごとに独立したバッファにする
	thread_local WCHAR buf[ 64 ];
	m_cv.Utf8ToUtf16(TUNER_NAME, buf, sizeof(buf) );
	
	return buf;
}

const BOOL CBonTuner::IsTunerOpening(void)
{
	DEBUG_CALL("");
	DEBUG_OUTPUT1("Called");

	// このBonDriverのOpenTuner()は同期処理のため、外部から観測できる
	// 「オープン処理中」という遷移状態は存在しない。
	// ホスト側がこの関数を「チューナーが開いているか」の判断に使うことがあるため、
	// 常にFALSEを返すのではなく実際の接続状態を反映する。
	return (conn != NULL) ? TRUE : FALSE;
}

LPCTSTR CBonTuner::EnumTuningSpace(const DWORD dwSpace)
{
	DEBUG_CALL("");

	// dwSpaceはDWORD(符号なし)のまま比較する。int32_tにキャストすると
	// 0x80000000以上が負数になって検査をすり抜け、g_pType[]を範囲外参照してしまう
	if (g_Max_Type < 0 || dwSpace > (DWORD)g_Max_Type) {
		return NULL;
	}

	// 使用可能なチューニング空間を返す
	const int len = 8;

	// スレッドごとに独立したバッファにして、複数スレッドからの同時呼び出しによる
	// 内容の競合・破損を防ぐ
	thread_local WCHAR buf[len];
	m_cv.Utf8ToUtf16( g_pType[dwSpace], buf, sizeof(buf) );

	return buf;
}

LPCTSTR CBonTuner::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	DEBUG_CALL("");

	if (g_Max_Type < 0 || dwSpace > (DWORD)g_Max_Type) {
		return NULL;
	}
	DWORD Bon_Channel = dwChannel + g_Channel_Base[dwSpace];
	if (Bon_Channel < g_Channel_Base[dwSpace]) {
		// dwChannelが巨大でDWORDがラップアラウンドした
		return NULL;
	}
	if (!g_Channel_JSON.contains(Bon_Channel)) {
		return NULL;
	}
	if (dwSpace < (DWORD)g_Max_Type) {
		if (Bon_Channel >= g_Channel_Base[dwSpace + 1]) {
			return NULL;
		}
	}

	try {
		picojson::object& channel_obj =
			g_Channel_JSON.get(Bon_Channel).get<picojson::object>();

		// 使用可能なチャンネル名を返す
		const int len = 128;

		// スレッドごとに独立したバッファにして、複数スレッドからの同時呼び出しによる
		// 内容の競合・破損を防ぐ
		thread_local WCHAR buf[len];
		m_cv.Utf8ToUtf16( channel_obj["name"].get<std::string>().c_str(), buf, sizeof(buf) );

		return buf;
	}
	catch (const std::exception& e) {
		// mirakcが想定外のJSON構造を返した場合に、例外をホスト側へ伝播させない
		ERROR_OUTPUT( "EnumChannelName: JSON access exception (%s)", e.what() );
		return NULL;
	}
}



const DWORD CBonTuner::GetCurSpace(void)
{
	DEBUG_CALL("");

	// 現在のチューニング空間を返す
	return m_dwCurSpace;
}

const DWORD CBonTuner::GetCurChannel(void)
{
	DEBUG_CALL("");

	// 現在のチャンネルを返す
	return m_dwCurChannel;
}

// チャンネル設定
const BOOL CBonTuner::SetChannel(const BYTE bCh)
{
	DEBUG_CALL("");

	return SetChannel((DWORD)0,(DWORD)bCh - 13);
}

// チャンネル設定
const BOOL CBonTuner::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	DEBUG_CALL("");
	DEBUG_OUTPUT("Start(sp:%d, ch:%d)", dwSpace, dwChannel);

	if (conn == NULL) {
		// OpenTuner() が成功していない状態で呼ばれた場合のNULL参照防止
		ERROR_OUTPUT1( "SetChannel: called while tuner is not open (conn is NULL)" );
		return FALSE;
	}

	// 引数の検証が済むまでは、受信中のストリームには触れない。
	// (先にshutdownしてしまうと、無効なチャンネル指定で FALSE を返しても
	//  実際には今のストリームだけが切れてしまう)
	if (g_Max_Type < 0 || dwSpace > (DWORD)g_Max_Type) {
		DEBUG_OUTPUT("end(failed) (sp:%d, spmax:%d)", dwSpace, g_Max_Type);
		return FALSE;
	}

	DWORD Bon_Channel = dwChannel + g_Channel_Base[dwSpace];
	if (Bon_Channel < g_Channel_Base[dwSpace]) {
		DEBUG_OUTPUT1("end(failed) (channel overflow)");
		return FALSE;
	}
	if (!g_Channel_JSON.contains(Bon_Channel)) {
		DEBUG_OUTPUT1("end(failed) (invalid channel)");
		return FALSE;
	}
	// dwChannel が次のチューニング空間にはみ出していないかチェック（EnumChannelNameと同様）
	if (dwSpace < (DWORD)g_Max_Type) {
		if (Bon_Channel >= g_Channel_Base[dwSpace + 1]) {
			DEBUG_OUTPUT1("end(failed) (channel out of space range)");
			return FALSE;
		}
	}

	// Server request
	const int len = 128;
	char url[len];
	try {
		picojson::object& channel_obj =
			g_Channel_JSON.get(Bon_Channel).get<picojson::object>();

		if (g_Service_Split == 1) {
			const int64_t id = (int64_t)channel_obj["id"].get<double>();
			snprintf(url, sizeof(url), "/api/services/%lld/stream?decode=%d", (long long int)id, g_DecodeB25);

		}
		else {
			const char *type = channel_obj["type"].get<std::string>().c_str();
			const char *channel = channel_obj["channel"].get<std::string>().c_str();
			snprintf(url, sizeof(url), "/api/channels/%s/%s/stream?decode=%d", type, channel, g_DecodeB25);

		}
	}
	catch (const std::exception& e) {
		// mirakcが想定外のJSON構造を返した場合に、例外をホスト側へ伝播させない
		ERROR_OUTPUT( "SetChannel: JSON access exception (%s)", e.what() );
		return FALSE;
	}
	DEBUG_OUTPUT( "request:url:%s", url);

	// 受信中のスレッドを停止して合流する。バッファ満杯で待機中でも確実に止まる
	// (waitForRecvThreadFinish内でソケットのshutdownとバッファ待機の中断を両方行う)
	waitForRecvThreadFinish();

	char szHeader[ 128 ];
	snprintf(szHeader, sizeof(szHeader), "Connection: close\r\nX-Mirakurun-Priority: %d\r\nUser-Agent: BonDriver_LinuxMirakc", g_Priority);
	char respHeader[ 512 ]; // todo
	int respCode = 0;
	int rc;
	rc = conn->sendGetRequest_WaitHeader( url, szHeader, respHeader, &respCode, sizeof(respHeader) );
	if( rc != 0 || respCode != 200 ) {
		ERROR_OUTPUT( "%s: Tuner unavailable (rc:%d, resp:%d)", g_TunerName, rc, respCode );
		conn->disconnect();
		// 失敗した場合、実際には(古いチャンネルも含め)どのチャンネルも受信できていない
		// 状態になる。ここでリセットしておかないと、直前に成功していた古いチャンネル
		// 番号がGetCurSpace()/GetCurChannel()から見え続けてしまい、ホスト側が
		// 「まだ受信中」と誤認する（特にリトライ時に問題になる）。
		m_dwCurSpace = 0xffffffff;
		m_dwCurChannel = 0xffffffff;
		return FALSE;
	}

	// チャンネル情報更新
	m_dwCurSpace = dwSpace;
	m_dwCurChannel = dwChannel;

	// TSデータパージ
	PurgeTsStream();

	// 受信スレッド起動
	int ret;
	ret = pthread_create( &m_hRecvThread, NULL, CBonTuner::RecvThread, (void *)this ); 
	if( ret != 0 ) {
		ERROR_OUTPUT( "pthread_create error %d", ret);
		// pthread_createが失敗した場合、*thread(m_hRecvThread)の値はPOSIX上「不定」となる。
		// 不定値のまま残すと、後続のwaitForRecvThreadFinish()やCloseTuner()が
		// 存在しないスレッドハンドルをpthread_joinしてしまい、ハング（デッドロック）や
		// 未定義動作の原因になる。明示的に「スレッドなし」を示す0にリセットする。
		m_hRecvThread = 0;
		// ストリームは実際には開始していないので、更新済みのチャンネル情報も戻す
		m_dwCurSpace = 0xffffffff;
		m_dwCurChannel = 0xffffffff;
		// ヘッダー受信までは成功して開いたままの接続を閉じる
		conn->disconnect();
		return FALSE;
	}
	m_bRecvThreadValid = true;

	DEBUG_OUTPUT1("End(succes)");
	return TRUE;
}

// 信号レベル(ビットレート)取得
const float CBonTuner::GetSignalLevel(void)
{
	DEBUG_CALL("");

	// チャンネル番号不明時は0を返す
	float fSignalLevel = 0;
	if (m_dwCurChannel != 0xffffffff && m_pGrabTsData)
		m_pGrabTsData->get_Bitrate(&fSignalLevel);

	return fSignalLevel;
}


/* private */
BOOL CBonTuner::InitChannel()
{
	// mirakc APIよりchannel取得
	if (!GetApiChannels(&g_Channel_JSON, g_Service_Split)) {
		return FALSE;
	}
	if (g_Channel_JSON.is<picojson::null>()) {
		return FALSE;
	}
	if (!g_Channel_JSON.contains(0)) {
		return FALSE;
	}

	try {
		// チューニング空間取得
		int i = 0;
		int j = -1;
		for (;;) {
			if (!g_Channel_JSON.contains(i)) {
				break;
			}
			picojson::object& channel_obj =
				g_Channel_JSON.get(i).get<picojson::object>();
			const char *type;
			if (g_Service_Split == 1) {
				picojson::object& channel_detail =
					channel_obj["channel"].get<picojson::object>();
				type = channel_detail["type"].get<std::string>().c_str();
			}
			else {
				type = channel_obj["type"].get<std::string>().c_str();
			}
			if (j < 0 || strcmp(g_pType[j], type)) {
				if (j + 1 >= SPACE_NUM) {
					// これ以上チューニング空間を作れない(最後の空間には
					// そのタイプの全チャンネルを割り当てるためここで打ち切る)
					break;
				}
				j++;
				int len = (int)strlen(type) + 1;
				g_pType[j] = (char *)malloc(len);
				if (!g_pType[j]) {
					j--;
					break;
				}
				strcpy(g_pType[j], type);
				g_Channel_Base[j] = i;
			}
			i++;
		}
		if (j < 0) {
			return FALSE;
		}
		g_Max_Type = j;
	}
	catch (const std::exception& e) {
		// mirakcが想定外のJSON構造を返した場合に、例外をホスト側へ伝播させない。
		// ここまでに確保していたg_pTypeがあれば解放してから抜ける。
		for (int k = 0; k < SPACE_NUM; k++) {
			if (g_pType[k]) {
				free(g_pType[k]);
				g_pType[k] = NULL;
			}
		}
		g_Max_Type = -1;
		ERROR_OUTPUT( "InitChannel: JSON access exception (%s)", e.what() );
		return FALSE;
	}

	return TRUE;
}

BOOL CBonTuner::GetApiChannels(picojson::value *channel_json, int service_split)
{
	const int len = 16;
	char url[len];

	::strcpy(url, "/api/");
	if (service_split == 1) {
		::strcat(url, "services");
	}
	else {
		::strcat(url, "channels");
	}

	char *data = NULL;
	int dwTotalSize = 0;

	if (!SendRequest(url, &data, &dwTotalSize)) {
		return FALSE;
	}

	*(data + dwTotalSize) = '\0';

	picojson::value v;
	std::string err = picojson::parse(v, data);
	if (!err.empty()) {
		free(data);
		return FALSE;
	}
	*channel_json = v;

	free(data);

	return TRUE;
}

BOOL CBonTuner::SendRequest(char *url, char **body, int *bodysize)
{
	int rc;

	char szHeader[ 128 ];
	snprintf(szHeader, sizeof(szHeader), "Connection: close\r\nX-Mirakurun-Priority: %d\r\nUser-Agent: BonDriver_LinuxMirakc", g_Priority);

	int respCode = 0;
	char respHeader[ 512 ]; // todo
	// チャンネル/サービス一覧の上限。本文は必要に応じて伸長して受信する
	const int bodymax = 32 * 1024 * 1024;

	DEBUG_OUTPUT( "request:url:%s", url);

	waitForRecvThreadFinish();

	*body = NULL;
	*bodysize = 0;
	rc = conn->sendGetRequest_WaitBody( url, szHeader, respHeader, &respCode, body, bodysize, bodymax, sizeof(respHeader) );
	if( rc != 0 || respCode != 200 ) {
		ERROR_OUTPUT( "%s: Tuner unavailable (rc:%d, resp:%d)", g_TunerName, rc, respCode );
		if( *body ) {
			free( *body );
			*body = NULL;
		}
		return FALSE;
	}

	return TRUE;
}


void *CBonTuner::RecvThread( void *pParam )
{
	CBonTuner *pThis = (CBonTuner *)pParam;

	#define BUF_SIZE (188 * 256)
	// 手動でのnew[]/delete[]は、将来コードが変更されて早期returnや例外が
	// 挟まった場合にリークしうる。RAII(std::vector)で確保することで、
	// どの経路で関数を抜けても確実に解放されるようにする。
	std::vector<char> buf_storage( BUF_SIZE );
	char *buf = buf_storage.data();

	int ret;
	
	for(;;) {
	
		// ここではソケットをclose()しない。close()は別スレッドのshutdown()と競合して
		// 再利用されたfdを誤って操作する恐れがあるため、スレッド終了後に
		// 制御側(SetChannel/CloseTuner)がdisconnect()する。
		ret = pThis->conn->recvBody( buf, BUF_SIZE );
		if( ret == 0 ) { // disconnect
			break;
		}
		else if( ret < 0 ) { // error
			ERROR_OUTPUT("recv error (%d)", ret );
			break;
		}

		if( !pThis->m_pGrabTsData->put_TsStream( (BYTE *)buf, ret ) ) {
			// RequestShutdown()によりバッファ待機が中断された（クローズ処理中）
			break;
		}

	}

	return 0;
}

void CBonTuner::waitForRecvThreadFinish(void)
{
	if( m_bRecvThreadValid ) {
		// ソケットI/O(recv())でのブロックを解除する
		if( conn ) {
			conn->shutdown();
		}
		// 受信スレッドがリングバッファ満杯でput_TsStream()のpthread_cond_waitに
		// 入っている場合、shutdown()では起きない。明示的に中断を知らせる。
		if( m_pGrabTsData ) {
			m_pGrabTsData->RequestShutdown();
		}
		pthread_join(m_hRecvThread, NULL);
		m_hRecvThread = 0;
		m_bRecvThreadValid = false;
		// 受信スレッドはもういないので、次のセッションのput_TsStream()が
		// 即座に失敗しないよう要求フラグをクリアする
		if( m_pGrabTsData ) {
			m_pGrabTsData->ResetShutdown();
		}
	}
}

