//------------------------------------------------------------------------------
// File: GrabTsData.h
//   Header file of GrabTsData
//   originaled by tkmsst
//   modified by matching
//------------------------------------------------------------------------------

//#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

//#include <atomic>
#include <pthread.h>
#include <new>

#define MAX_PATH 256

#define CEIL(a,b) (((a)+(b)-1)/(b))

// TS data buffer size
//
#define DATA_BUF_SIZE (188 * 256)
#define RING_BUF_SIZE (DATA_BUF_SIZE * 512) // > 24Mbps / 8bit * 5sec

// GrabTsData class
//
class GrabTsData
{
public:
	// Constructor
//	GrabTsData(HANDLE *phOnStreamEvent)
	GrabTsData()
	{
//		m_phOnStreamEvent = phOnStreamEvent;
		m_nAccumData = 0;
		m_bPurge = FALSE;
		m_nPush = 0;
		m_nPull = 0;
		m_dBitrate = 0;
		m_bReading = false;
		m_bShutdown = false;
		m_pDst = (BYTE *)malloc(DATA_BUF_SIZE);
		m_pBuf = (BYTE *)malloc(RING_BUF_SIZE);
		if (!m_pDst || !m_pBuf) {
			// NULLのまま使うとmemcpyでクラッシュするため、構築失敗として扱う
			// (コンストラクタで例外を投げるとデストラクタは呼ばれないのでここで解放する)
			::free(m_pDst);
			::free(m_pBuf);
			throw std::bad_alloc();
		}

		// Wait_TsStream()のタイムアウト計算はCLOCK_MONOTONICを使う。
		// デフォルト(CLOCK_REALTIME)だと、NTP補正やシステム時刻の手動変更で
		// 壁時計が前後に跳躍した場合、想定より大幅に長く待ってしまったり
		// 即座にタイムアウト扱いになったりする。
		pthread_condattr_t condattr;
		::pthread_condattr_init(&condattr);
		::pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
		::pthread_cond_init( &m_phOnStreamEvent, &condattr );
		::pthread_condattr_destroy(&condattr);

		::pthread_cond_init( &m_phOnStreamGetEvent, NULL );
//		::pthread_mutex_init( &m_pRingMutex, NULL );

		pthread_mutexattr_t attr;
		::pthread_mutexattr_init(&attr); // settype前に必ず初期化する必要がある（未初期化のまま使うのは未定義動作）
		::pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		::pthread_mutex_init( &m_pRingMutex, &attr );
		::pthread_mutexattr_destroy(&attr); // mutex_initに反映済みなので破棄してよい

		m_ui64LastTime = GetTickCount64();
	}
	// Destructor
	~GrabTsData()
	{
		if (m_pBuf) {
			::free(m_pBuf);
		}
		if (m_pDst) {
			::free(m_pDst);
		}

		::pthread_mutex_destroy( &m_pRingMutex );
		::pthread_cond_destroy( &m_phOnStreamEvent );
		::pthread_cond_destroy( &m_phOnStreamGetEvent );
	}
	// Interfaces
	BOOL put_TsStream(BYTE *pSrc, DWORD dwSize);
	BOOL get_TsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain);
	BOOL purge_TsStream(void);
	BOOL get_ReadyCount(DWORD *pdwRemain);
	BOOL get_Bitrate(float *pfBitrate);

	DWORD Wait_TsStream(DWORD waitMs);

	// CloseTuner()等でput_TsStream()の待機(バッファ満杯待ち)を確実に中断させるための
	// シャットダウン要求。conn->shutdown()はソケットI/Oのみに作用し、
	// pthread_cond_waitでブロックしている送信スレッドは起こせないため、
	// 別途これで明示的に知らせる必要がある。
	void RequestShutdown(void);
	// 新しい受信セッション開始前に、前回のシャットダウン要求フラグをクリアする
	void ResetShutdown(void);

private:
	uint64_t GetTickCount64();
	// Stream event
//	HANDLE *m_phOnStreamEvent;
	pthread_cond_t m_phOnStreamEvent;
	pthread_cond_t m_phOnStreamGetEvent;
	pthread_mutex_t m_pRingMutex;
	
	// Bitrate calculation
#if 0
	std::atomic_long m_nAccumData;
	// Purge flag
	std::atomic_bool m_bPurge;
	// TS data buffer (simple ring buffer)
	std::atomic_ulong m_nPush;
	std::atomic_ulong m_nPull;
#endif
	long m_nAccumData;
	// Purge flag
	bool m_bPurge;
	// TS data buffer (simple ring buffer)
	unsigned long m_nPush;
	unsigned long m_nPull;

	// get_TsStream()は宛先バッファm_pDstへのmemcpyをロック解除区間で行う
	// （送信スレッドを長時間ブロックしないための最適化）。プロデューサー
	// (put_TsStream)との競合はm_nPullの更新タイミングにより安全だが、
	// get_TsStream()自体が複数スレッドから同時に呼ばれると、共有の
	// m_pDstに対して同時にmemcpyしてしまい内容が競合・破損する。
	// これを検出して安全に拒否するためのフラグ。
	bool m_bReading;

	// CloseTuner()実行中に、バッファ満杯でput_TsStream()内のpthread_cond_waitに
	// ブロックしたままの送信スレッドを確実に脱出させるためのフラグ。
	bool m_bShutdown;

	// ビットレート計算用の状態。関数内staticにすると全インスタンス・全呼び出しで
	// 共有されてしまい、ロックなしで読み書きされるとデータ競合になるため、
	// インスタンスメンバとして持ち、m_pRingMutexで保護してアクセスする。
	double m_dBitrate;
	uint64_t m_ui64LastTime;

	BYTE *m_pDst;
	BYTE *m_pBuf;
};
