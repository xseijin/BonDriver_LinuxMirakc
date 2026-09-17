// char_code_conv.hpp
// originaled by nns779
// modified by matching

#pragma once

#include <memory>
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
};

