// SPDX-License-Identifier: MIT
/*
 * INI file loader (config.cpp)
 *
 * Copyright (c) 2021 nns779
 * modified by matching
 */

#include "config.hpp"

#include <cstring>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "util.hpp"

bool Config::Section::Exists(const std::string& key) const noexcept
{
	return !!data_.count(key);
}

bool Config::Section::Set(const std::string& key, const std::string& value)
{
	return data_.emplace(key, value).second;
}

const std::string& Config::Section::Get(const std::string& key) const
{
	return data_.at(key);
}

std::string Config::Section::Get(const std::string& key, const std::string& default_value) const
{
	try {
		return Get(key);
	} catch (const std::out_of_range&) {
		return default_value;
	}
}

int Config::Section::Get(const std::string& key, int default_value) const
{
	try {
		// 基数0(自動判定)だと "010" が8進数の8、"08080" が0になってしまう。
		// 先頭の0を含む10進表記をそのまま読めるよう、10進固定とし、
		// 明示的な "0x" 接頭辞のみ16進として扱う。
		const std::string& v = Get(key);
		std::size_t i = 0;
		while (i < v.size() && (v[i] == ' ' || v[i] == '\t'))
			i++;
		std::size_t j = i;
		if (j < v.size() && (v[j] == '+' || v[j] == '-'))
			j++;
		int base = 10;
		if (j + 1 < v.size() && v[j] == '0' && (v[j + 1] == 'x' || v[j + 1] == 'X'))
			base = 16;
		return std::stoi(v, nullptr, base);
	} catch (const std::out_of_range&) {
		return default_value;
	} catch (const std::invalid_argument&) {
		return default_value;
	}
}

bool Config::Load(const std::string& path)
{
	std::ifstream ifs(path);
	Config::Section *sct = nullptr;

	if (!ifs.is_open())
		return false;

	std::string line_str;
	std::vector<char> line_buf;

	// 行の長さに上限を設けない(固定長バッファだと長い行でfailbitが立ち、
	// 以降の設定が黙って読まれなくなる)
	while (std::getline(ifs, line_str)) {
		line_buf.assign(line_str.begin(), line_str.end());
		line_buf.push_back('\0');

		char *p = line_buf.data();
		std::size_t len = std::strlen(p);

		// CRLFの'\r'は、空行判定や前後の空白除去より先に取り除く
		while (len > 0 && p[len - 1] == '\r') {
			p[--len] = '\0';
		}

		util::Trim(&p, &len);
		if (!len) {
			// blank line
			continue;
		}

		switch (*p) {
		case ';':
		case '#':
			// comment
			continue;

		case '[':
		{
			// section name

			p++;
			len--;

			util::Trim(&p, &len);
			if (!len) {
				// invalid section name
				sct = nullptr;
				continue;
			}

			char *term = std::strchr(p + 1, ']');
			if (!term)
				term = p + len;

			// term は "]" の位置そのもの（1つ先ではない）を指す場合があるため、
			// RTrim には p〜term 間の実際の長さを渡す（len をそのまま渡すと
			// "]" 以降の文字数が余分に含まれ、RTrim が p より手前まで
			// 走査してバッファ外を読みに行く可能性がある）
			std::size_t seg_len = (std::size_t)(term - p);

			util::RTrim(&term, &seg_len);
			if (!seg_len) {
				// invalid section name
				sct = nullptr;
				continue;
			}

			auto s = sections_.emplace(p, Config::Section());
			sct = (s.second) ? &s.first->second : nullptr;

			break;
		}

		default:
		{
			// key-value pair

			if (!sct)
				break;

			char *key = p, *key_term = std::strchr(p, '='), *val;
			if (key_term) {
				*key_term = '\0';
				val = key_term + 1;
			} else {
				key_term = val = key + len;
			}

			std::size_t key_len = std::strlen(key), val_len = std::strlen(val);
			char *val_term = val + val_len;

			util::Trim(&key, &key_len);
			util::RTrim(&key_term, &key_len);

			util::Trim(&val, &val_len);

			if (val_len > 0 && (val[0] == '\"' || val[0] == '\'')) {
				// 引用符付きの値。閉じ引用符までを値とし、その後ろ(インラインコメント等)は無視する
				char *close = std::strchr(val + 1, val[0]);
				if (close) {
					*close = '\0';
					val++;
					val_len = (std::size_t)(close - val);
				} else {
					util::RTrim(&val_term, &val_len);
				}
			} else {
				// 引用符なしの値。空白の後ろの ';' または '#' 以降はインラインコメント
				for (std::size_t k = 1; k < val_len; k++) {
					if ((val[k] == ';' || val[k] == '#') && (val[k - 1] == ' ' || val[k - 1] == '\t')) {
						val[k] = '\0';
						val_len = k;
						val_term = val + k;
						break;
					}
				}
				util::RTrim(&val_term, &val_len);
			}

			sct->Set(key, val);
			break;
		}

		}
	}

	return true;
}

bool Config::Exists(const std::string& sct) const noexcept
{
	return !!sections_.count(sct);
}

const Config::Section& Config::Get(const std::string& sct) const
{
	return sections_.at(sct);
}
