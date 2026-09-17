//------------------------------------------------------------------------------
// File: GrabTsData.cpp
//   Implementation of GrabTsData
//
//   originaled by tkmsst
//   modified by matching
//------------------------------------------------------------------------------
#include "type_compat.h"
#include "GrabTsData.hpp"

#include "logoutput.hpp"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <algorithm>

// Put TS data into the ring buffer
//
BOOL GrabTsData::put_TsStream(BYTE *pSrc, DWORD dwSize)
{
	if (dwSize < 1)
		return FALSE;

	DWORD nPush, nPull;
	DWORD nTail;
	DWORD nAvail;

	::pthread_mutex_lock( &m_pRingMutex );
	{
		// バッファに空きができるまで待つ
		for(;;) {
			nPush = m_nPush;
			nPull = m_nPull;
			nAvail = (RING_BUF_SIZE - 1) - (RING_BUF_SIZE + nPush - nPull) % RING_BUF_SIZE;
			// nPush/nPullが常に[0, RING_BUF_SIZE)に収まっている限り理論上は
			// 起こらないが、万一この不変条件が崩れた場合、符号なし演算の
			// アンダーフローでnAvailが巨大な値(約4GB)になり、後続のmemcpyで
			// ヒープバッファオーバーフローを起こしかねない。安全側にクランプする。
			if( nAvail > (DWORD)(RING_BUF_SIZE - 1) ) {
				nAvail = 0;
			}
			if( nAvail >= dwSize ) break;
			if( m_bShutdown ) {
				// シャットダウン要求あり。誰もget_TsStream()を呼ばず
				// バッファに空きができる見込みがない状況で永久に待たない。
				::pthread_mutex_unlock( &m_pRingMutex );
				return FALSE;
			}
			::pthread_cond_wait( &m_phOnStreamGetEvent, &m_pRingMutex );
		}
		m_nAccumData += dwSize;
		// mutexを保持したままコピーしてインデックスを更新する
		nTail = RING_BUF_SIZE - nPush;
		if( dwSize <= nTail ) {
			::memcpy( m_pBuf + nPush, pSrc, dwSize );
			nPush += dwSize;
			if( nPush >= RING_BUF_SIZE ) nPush = 0;
		} else {
			::memcpy( m_pBuf + nPush, pSrc, nTail );
			::memcpy( m_pBuf, pSrc + nTail, dwSize - nTail );
			nPush = dwSize - nTail;
		}
		m_nPush = nPush;
		::pthread_cond_signal( &m_phOnStreamEvent );
	}
	::pthread_mutex_unlock( &m_pRingMutex );

	return TRUE;
}

// Get TS data from the ring buffer
//
BOOL GrabTsData::get_TsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	DWORD nPush, nPull;

	::pthread_mutex_lock( &m_pRingMutex );
	{
		// 実際のリングバッファリセットはpurge_TsStream()側で即座に行われている。
		// ここでは「パージ直後である」ことを呼び出し元に一度だけ知らせる。
		if( m_bPurge ) {
			m_bPurge = FALSE;
			::pthread_mutex_unlock( &m_pRingMutex );
			return FALSE;
		}

		if( m_bReading ) {
			// 既に別の呼び出しがロック解除区間でmemcpy中。
			// 宛先バッファm_pDstは共有のため、同時に書き込むと内容が
			// 競合・破損するので安全に拒否する。
			::pthread_mutex_unlock( &m_pRingMutex );
			return FALSE;
		}
		m_bReading = true;

		// lock push and pull positions
		nPush = m_nPush;
		nPull = m_nPull;
	}
	::pthread_mutex_unlock( &m_pRingMutex );

	// copy TS data to the destination buffer
	DWORD nTail = RING_BUF_SIZE - nPull; // size between the current position and the buffer end
	DWORD nData = (RING_BUF_SIZE + nPush - nPull) % RING_BUF_SIZE; // size of TS data stored
	DWORD nRemain = nData;
	if (nData > 0) {
		nData = std::min(nData, (DWORD)DATA_BUF_SIZE);
		if (nData < nTail) {
			//CopyMemory(m_pDst, m_pBuf + nPull, nData);
			memcpy(m_pDst, m_pBuf + nPull, nData);
			nPull += nData;
		} else {
			//CopyMemory(m_pDst, m_pBuf + nPull, nTail);
			//CopyMemory(m_pDst + nTail, m_pBuf, nData - nTail);
			memcpy(m_pDst, m_pBuf + nPull, nTail);
			memcpy(m_pDst + nTail, m_pBuf, nData - nTail);
			nPull = nData - nTail;
		}
		nRemain -= nData;
	}

	// update the pull position, and release the "reading" guard
	::pthread_mutex_lock( &m_pRingMutex );
	{
		if( m_bPurge ) {
			// memcpy中(ロック解除区間)の間にpurge_TsStream()が実行され、
			// リングバッファが即座にリセットされている。ここで保持している
			// nPullはパージ前の古い値であり、これを書き戻すとリセット直後の
			// m_nPullを壊してしまう（m_nPushだけ0でm_nPullが古い値のまま、
			// という矛盾した状態になり、以降のnData計算が破綻する）。
			// そのため今回の読み出し結果は破棄し、m_nPullには一切触れない。
			m_bReading = false;
			::pthread_mutex_unlock( &m_pRingMutex );
			return FALSE;
		}

		if (nData > 0) {
		//		std::atomic_store(&m_nPull, nPull);
			m_nPull = nPull;
			::pthread_cond_signal( &m_phOnStreamGetEvent );
		}
		m_bReading = false;
	}
	::pthread_mutex_unlock( &m_pRingMutex );

	// set destination variables
	if (ppDst) {
		*ppDst = m_pDst;
	}
	if (pdwSize) {
		*pdwSize = nData;
	}
	if (pdwRemain) {
		*pdwRemain = CEIL(nRemain, DATA_BUF_SIZE);
	}

	return TRUE;
}

// Purge TS data
//
BOOL GrabTsData::purge_TsStream(void)
{
	::pthread_mutex_lock( &m_pRingMutex );
	{
		// リングバッファは即座にリセットする。get_TsStream()が呼ばれるまで
		// 遅延させると、その間に送信スレッドがバッファ満杯でput_TsStream()の
		// pthread_cond_waitでブロックしたまま誰にも起こされず、ホストが
		// しばらくget_TsStream()を呼ばない場合に事実上のデッドロック
		// （無期限のWait継続）に陥る可能性がある。
		m_nPull = m_nPush = 0;
		m_nAccumData = 0; // reset bitrate
		m_bPurge = TRUE; // 次回get_TsStream()呼び出し時に「パージ直後」を1回だけ知らせる
		// バッファに空きができたことを、待機中のput_TsStream()へ即座に知らせる
		::pthread_cond_signal( &m_phOnStreamGetEvent );
	}
	::pthread_mutex_unlock( &m_pRingMutex );

	return TRUE;
}

// CloseTuner()等から呼び、put_TsStream()内でバッファ満杯のまま
// pthread_cond_waitしている送信スレッドを確実に脱出させる
//
void GrabTsData::RequestShutdown(void)
{
	::pthread_mutex_lock( &m_pRingMutex );
	{
		m_bShutdown = true;
		::pthread_cond_signal( &m_phOnStreamGetEvent );
	}
	::pthread_mutex_unlock( &m_pRingMutex );
}

// 新しい受信セッションを開始する前に、前回のシャットダウン要求をクリアする
//
void GrabTsData::ResetShutdown(void)
{
	::pthread_mutex_lock( &m_pRingMutex );
	{
		m_bShutdown = false;
	}
	::pthread_mutex_unlock( &m_pRingMutex );
}

// Get the number of TS data blocks in the ring buffer
//
BOOL GrabTsData::get_ReadyCount(DWORD *pdwRemain)
{
	if (pdwRemain) {
		DWORD nPush, nPull;
		::pthread_mutex_lock( &m_pRingMutex );
		{
			nPush = m_nPush;
			nPull = m_nPull;
		}
		::pthread_mutex_unlock( &m_pRingMutex );
	
		*pdwRemain = CEIL((RING_BUF_SIZE + nPush - nPull) % RING_BUF_SIZE, DATA_BUF_SIZE);
	}

	return TRUE;
}

uint64_t GrabTsData::GetTickCount64()
{
	
    struct timespec ts;
    uint64_t theTick = 0U;

	// ビットレート計算での経過時間測定にのみ使うため、壁時計(CLOCK_REALTIME)ではなく
	// システム時刻の変更やNTP補正の影響を受けないCLOCK_MONOTONICを使う。
	// CLOCK_REALTIMEのままだと、時刻が巻き戻った場合にui64Now - m_ui64LastTime
	// (unsigned同士の引き算)が桁あふれし、異常なビットレート値を計算してしまう。
	clock_gettime( CLOCK_MONOTONIC, &ts );
    theTick  = ts.tv_nsec / 1000000;
    theTick += ts.tv_sec * 1000;
	
    return theTick;
}

// Calculate bitrate
//
BOOL GrabTsData::get_Bitrate(float *pfBitrate)
{
	uint64_t ui64Now = GetTickCount64(); // ms
	double dBitrate;

	// m_dBitrate/m_ui64LastTimeは関数内staticではなくインスタンスメンバとして持ち、
	// 読み書きをすべて同じロック区間内で行うことで、複数スレッドから同時に
	// 呼ばれた場合のデータ競合を避ける。
	::pthread_mutex_lock( &m_pRingMutex );
	{
		uint64_t ui64Duration = ui64Now - m_ui64LastTime;
		if (ui64Duration >= 1000) {
			m_dBitrate = m_nAccumData / (double)ui64Duration * 8 * 1000 / 1024 / 1024.0; // Mbps
			m_nAccumData = 0;
			m_ui64LastTime = ui64Now;
		}
		dBitrate = m_dBitrate;
	}
	::pthread_mutex_unlock( &m_pRingMutex );

	*pfBitrate = (float)std::min(dBitrate, (double)100);

	return TRUE;
}


DWORD GrabTsData::Wait_TsStream(DWORD waitMs)
{
	DWORD cnt;
	DWORD ret = WAIT_TIMEOUT;
	int r;

	struct timespec to;

	// CLOCK_MONOTONICで絶対デッドラインを計算する
	// （このコンドバーはCLOCK_MONOTONIC属性で初期化済み）。
	// time(NULL)（CLOCK_REALTIME）を使うと、NTP補正やシステム時刻の手動変更で
	// 壁時計が前後に跳躍した際、想定より大幅に長く待ってしまったり
	// 即座にタイムアウト扱いになったりする。
	clock_gettime( CLOCK_MONOTONIC, &to );
	to.tv_sec += waitMs / 1000;
	to.tv_nsec += (long)(waitMs % 1000) * 1000 * 1000;
	// 現在の計算では合計は最大でも 1,998,999,999 (< 2*10^9) にしかならないため
	// 1回のif判定で正規化しきれるが、将来この式が変わっても安全なようにwhileにしておく
	while( to.tv_nsec >= 1000000000L ) {
		to.tv_sec += 1;
		to.tv_nsec -= 1000000000L;
	}
	
    ::pthread_mutex_lock( &m_pRingMutex );
	{
		get_ReadyCount( &cnt );
		if( cnt == 0 ){
			r = ::pthread_cond_timedwait( &m_phOnStreamEvent, &m_pRingMutex, &to );
			if( r == 0 ) { // success
				ret = WAIT_OBJECT_0;
			}
			else if( errno == ETIMEDOUT ) {
				ret = WAIT_TIMEOUT;
			}
			else {
				ret = WAIT_TIMEOUT;
			}
		}
		else {
			// 既にデータがある場合は即座にOBJECT_0を返す
			ret = WAIT_OBJECT_0;
		}
	}
	::pthread_mutex_unlock( &m_pRingMutex );

	return ret;
}

