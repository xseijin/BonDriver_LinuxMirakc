// char_code_conv.hpp
// originaled by nns779
// modified by matching

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <iconv.h>

#include "type_compat.h"

class CharCodeConv final {
public:
	CharCodeConv();
	~CharCodeConv();

	// cannot copy
	CharCodeConv(const CharCodeConv&) = delete;
	CharCodeConv& operator=(const CharCodeConv&) = delete;

	bool Utf8ToUtf16(const char *src, WCHAR *dst, size_t dst_size_bytes);

private:
	::iconv_t cd_;
	// iconv_tは内部状態を持つため、複数スレッドから同時に使うと競合する
	std::mutex mtx_;
};

