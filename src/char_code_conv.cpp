// SPDX-License-Identifier: MIT
/*
 * Character code converter (char_code_conv.cpp)
 *
 * Copyright (c) 2021 nns779
 * modified by matching
 */

#include "char_code_conv.hpp"

#include <stdexcept>
#include <string.h>
#include <errno.h>

CharCodeConv::CharCodeConv()
{
	cd_ = ::iconv_open("UTF-16LE", "UTF-8");
	if (cd_ == reinterpret_cast<::iconv_t>(-1))
		throw std::runtime_error("CharCodeConv::CharCodeConv: ::iconv_open() failed");
}

CharCodeConv::~CharCodeConv()
{
	if (cd_ != reinterpret_cast<::iconv_t>(-1))
		::iconv_close(cd_);
}

bool CharCodeConv::Utf8ToUtf16(const char *src, WCHAR *dst, size_t dst_size_bytes)
{
	char *s = (char *)src;
	size_t s_len = ::strlen( src );

	// 呼び出し側バッファを超えて書き込まない。ヌル終端用に最低1 WCHAR分は残す
	if( dst_size_bytes < sizeof(WCHAR) ) {
		return false;
	}

	memset( dst, 0, dst_size_bytes );

	size_t d_len = dst_size_bytes - sizeof(WCHAR); // ヌル終端分を確保
	size_t cr = ::iconv(cd_, &s, &s_len, (char **)&dst, &d_len);
	if (cr == (size_t)-1 && errno != E2BIG)
		return false;

	// E2BIGの場合は入りきる分だけ変換され、残りは切り捨てられる（バッファ保護優先）
	return true;
}

