// Minimal JSON reader for the AdShield checker (dev tool, not app code).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AdShieldCheck {

struct Json {
	enum class Type {
		Null,
		Bool,
		Int,
		Real,
		String,
		Array,
		Object,
	};

	Type type = Type::Null;
	bool boolean = false;
	std::int64_t integer = 0;
	double real = 0.;
	std::u32string string;
	std::vector<Json> array;
	std::vector<std::pair<std::string, Json>> object;

	[[nodiscard]] const Json *find(std::string_view key) const {
		for (const auto &[k, v] : object) {
			if (k == key) {
				return &v;
			}
		}
		return nullptr;
	}
	[[nodiscard]] bool has(std::string_view key) const {
		const auto value = find(key);
		return value && value->type != Type::Null;
	}
	[[nodiscard]] const Json &operator[](std::string_view key) const {
		static const auto kNull = Json();
		const auto value = find(key);
		return value ? *value : kNull;
	}
	[[nodiscard]] bool isNull() const {
		return type == Type::Null;
	}
	[[nodiscard]] std::int64_t asInt(std::int64_t fallback = 0) const {
		if (type == Type::Int) {
			return integer;
		} else if (type == Type::Real) {
			return std::int64_t(real);
		} else if (type == Type::Bool) {
			return boolean ? 1 : 0;
		}
		return fallback;
	}
	[[nodiscard]] bool truthy() const {
		switch (type) {
		case Type::Null: return false;
		case Type::Bool: return boolean;
		case Type::Int: return integer != 0;
		case Type::Real: return real != 0.;
		case Type::String: return !string.empty();
		case Type::Array: return !array.empty();
		case Type::Object: return !object.empty();
		}
		return false;
	}
	[[nodiscard]] std::u32string asString() const {
		return (type == Type::String) ? string : std::u32string();
	}
};

class JsonParser final {
public:
	explicit JsonParser(std::string_view text) : _text(text) {
	}

	[[nodiscard]] bool parse(Json &out) {
		skip();
		if (!value(out, 0)) {
			return false;
		}
		skip();
		return _position == _text.size();
	}

private:
	void skip() {
		while (_position < _text.size()) {
			const auto c = _text[_position];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
				++_position;
			} else {
				break;
			}
		}
	}

	[[nodiscard]] bool literal(std::string_view word) {
		if (_text.substr(_position, word.size()) != word) {
			return false;
		}
		_position += word.size();
		return true;
	}

	[[nodiscard]] bool value(Json &out, int depth) {
		if (depth > 64 || _position >= _text.size()) {
			return false;
		}
		const auto c = _text[_position];
		if (c == '{') {
			return objectValue(out, depth);
		} else if (c == '[') {
			return arrayValue(out, depth);
		} else if (c == '"') {
			out.type = Json::Type::String;
			return stringValue(out.string);
		} else if (c == 't') {
			out.type = Json::Type::Bool;
			out.boolean = true;
			return literal("true");
		} else if (c == 'f') {
			out.type = Json::Type::Bool;
			out.boolean = false;
			return literal("false");
		} else if (c == 'n') {
			out.type = Json::Type::Null;
			return literal("null");
		}
		return numberValue(out);
	}

	[[nodiscard]] bool objectValue(Json &out, int depth) {
		out.type = Json::Type::Object;
		++_position;
		skip();
		if (_position < _text.size() && _text[_position] == '}') {
			++_position;
			return true;
		}
		while (true) {
			skip();
			auto key = std::u32string();
			if (_position >= _text.size()
				|| _text[_position] != '"'
				|| !stringValue(key)) {
				return false;
			}
			skip();
			if (_position >= _text.size() || _text[_position] != ':') {
				return false;
			}
			++_position;
			skip();
			auto item = Json();
			if (!value(item, depth + 1)) {
				return false;
			}
			auto narrow = std::string();
			for (const auto c : key) {
				narrow.push_back(char(c));
			}
			out.object.emplace_back(std::move(narrow), std::move(item));
			skip();
			if (_position < _text.size() && _text[_position] == ',') {
				++_position;
				continue;
			}
			if (_position < _text.size() && _text[_position] == '}') {
				++_position;
				return true;
			}
			return false;
		}
	}

	[[nodiscard]] bool arrayValue(Json &out, int depth) {
		out.type = Json::Type::Array;
		++_position;
		skip();
		if (_position < _text.size() && _text[_position] == ']') {
			++_position;
			return true;
		}
		while (true) {
			skip();
			auto item = Json();
			if (!value(item, depth + 1)) {
				return false;
			}
			out.array.push_back(std::move(item));
			skip();
			if (_position < _text.size() && _text[_position] == ',') {
				++_position;
				continue;
			}
			if (_position < _text.size() && _text[_position] == ']') {
				++_position;
				return true;
			}
			return false;
		}
	}

	[[nodiscard]] bool hex4(std::uint32_t &out) {
		if (_text.size() - _position < 4) {
			return false;
		}
		out = 0;
		for (auto i = 0; i != 4; ++i) {
			const auto c = _text[_position++];
			out <<= 4;
			if (c >= '0' && c <= '9') {
				out |= std::uint32_t(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				out |= std::uint32_t(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				out |= std::uint32_t(c - 'A' + 10);
			} else {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool stringValue(std::u32string &out) {
		++_position;
		while (_position < _text.size()) {
			const auto c = static_cast<unsigned char>(_text[_position]);
			if (c == '"') {
				++_position;
				return true;
			} else if (c == '\\') {
				++_position;
				if (_position >= _text.size()) {
					return false;
				}
				const auto e = _text[_position++];
				switch (e) {
				case '"': out.push_back(U'"'); break;
				case '\\': out.push_back(U'\\'); break;
				case '/': out.push_back(U'/'); break;
				case 'b': out.push_back(U'\b'); break;
				case 'f': out.push_back(U'\f'); break;
				case 'n': out.push_back(U'\n'); break;
				case 'r': out.push_back(U'\r'); break;
				case 't': out.push_back(U'\t'); break;
				case 'u': {
					auto unit = std::uint32_t();
					if (!hex4(unit)) {
						return false;
					}
					if (unit >= 0xD800 && unit <= 0xDBFF
						&& _text.substr(_position, 2) == "\\u") {
						const auto save = _position;
						_position += 2;
						auto low = std::uint32_t();
						if (hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
							unit = 0x10000
								+ ((unit - 0xD800) << 10)
								+ (low - 0xDC00);
						} else {
							_position = save;
						}
					}
					out.push_back(char32_t(unit));
				} break;
				default: return false;
				}
			} else {
				auto cp = std::uint32_t(c);
				auto length = 1;
				if (c >= 0xF0) {
					cp = c & 0x07;
					length = 4;
				} else if (c >= 0xE0) {
					cp = c & 0x0F;
					length = 3;
				} else if (c >= 0xC0) {
					cp = c & 0x1F;
					length = 2;
				}
				if (_text.size() - _position < std::size_t(length)) {
					return false;
				}
				for (auto i = 1; i < length; ++i) {
					cp = (cp << 6)
						| (static_cast<unsigned char>(_text[_position + i]) & 0x3F);
				}
				_position += length;
				out.push_back(char32_t(cp));
			}
		}
		return false;
	}

	[[nodiscard]] bool numberValue(Json &out) {
		const auto from = _position;
		auto real = false;
		while (_position < _text.size()) {
			const auto c = _text[_position];
			if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
				++_position;
			} else if (c == '.' || c == 'e' || c == 'E') {
				real = true;
				++_position;
			} else {
				break;
			}
		}
		if (_position == from) {
			return false;
		}
		const auto text = std::string(_text.substr(from, _position - from));
		if (real) {
			out.type = Json::Type::Real;
			out.real = std::strtod(text.c_str(), nullptr);
			return true;
		}
		out.type = Json::Type::Int;
		auto negative = false;
		auto i = std::size_t(0);
		if (text[0] == '-') {
			negative = true;
			i = 1;
		}
		auto value = std::uint64_t(0);
		for (; i != text.size(); ++i) {
			if (text[i] < '0' || text[i] > '9') {
				return false;
			}
			value = value * 10 + std::uint64_t(text[i] - '0');
		}
		out.integer = negative
			? std::int64_t(0 - value)
			: std::int64_t(value);
		return true;
	}

	std::string_view _text;
	std::size_t _position = 0;

};

} // namespace AdShieldCheck
