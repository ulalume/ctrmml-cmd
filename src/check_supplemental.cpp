#include "check_supplemental.h"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace
{
using ctrmml_cmd::CheckMessage;
using ctrmml_cmd::SupplementalChecker;

using SourceSpan = SupplementalChecker::SourceSpan;
using TagDefinition = SupplementalChecker::TagDefinition;
using TagToken = SupplementalChecker::TagToken;

bool is_blank_char(char c)
{
	return std::isblank(static_cast<unsigned char>(c)) != 0;
}

bool is_track_id_start(char c)
{
	return (c >= 'A' && c <= 'Z') || std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '*';
}

bool is_instrument_key(const std::string &key, uint16_t &id)
{
	if (key.size() < 2 || key[0] != '@')
		return false;
	unsigned long value = 0;
	for (size_t i = 1; i < key.size(); ++i)
	{
		if (!std::isdigit(static_cast<unsigned char>(key[i])))
			return false;
		value = (value * 10) + static_cast<unsigned long>(key[i] - '0');
		if (value > 0xfffful)
			return false;
	}
	id = static_cast<uint16_t>(value);
	return true;
}

CheckMessage make_message(const std::string &display_name,
													const SourceSpan &span,
													const std::string &message,
													const std::string &code)
{
	CheckMessage out{};
	out.path = display_name;
	out.line = span.line;
	out.col = span.col;
	out.length = span.length;
	out.message = message;
	out.code = code;
	return out;
}

SourceSpan preferred_span(const TagToken *token, const SourceSpan &fallback)
{
	if (token != nullptr)
	{
		if (token->outer_span.line != 0 && token->outer_span.col != 0 && token->outer_span.length != 0)
			return token->outer_span;
		if (token->span.line != 0 && token->span.col != 0 && token->span.length != 0)
			return token->span;
	}
	return fallback;
}

std::string decode_quoted(std::string_view raw)
{
	std::string out;
	out.reserve(raw.size());
	for (size_t i = 0; i < raw.size(); ++i)
	{
		char c = raw[i];
		if (c == '\\' && i + 1 < raw.size())
		{
			++i;
			char escaped = raw[i];
			if (escaped == 'n')
				out.push_back('\n');
			else if (escaped == 't')
				out.push_back('\t');
			else
				out.push_back(escaped);
			continue;
		}
		out.push_back(c);
	}
	return out;
}

void tokenize_tag_values(TagDefinition &tag,
												 std::string_view text,
												 uint32_t line,
												 uint32_t start_col)
{
	size_t i = 0;
	char last_separator = 0;
	while (i < text.size())
	{
		size_t next = text.find_first_of(" \t\r\n\",;", i);
		if (next == std::string_view::npos)
		{
			if (text.size() > i)
			{
				tag.tokens.push_back(TagToken{
						std::string(text.substr(i)),
						SourceSpan{line, start_col + static_cast<uint32_t>(i), static_cast<uint32_t>(text.size() - i)},
						SourceSpan{line, start_col + static_cast<uint32_t>(i), static_cast<uint32_t>(text.size() - i)},
						false,
				});
			}
			break;
		}

		char separator = text[next];
		if (separator == '"' && next == i)
		{
			size_t j = i + 1;
			while (j < text.size())
			{
				if (text[j] == '\\' && j + 1 < text.size())
				{
					j += 2;
					continue;
				}
				if (text[j] == '"')
					break;
				++j;
			}

			const size_t content_begin = i + 1;
			const size_t content_end = (j < text.size() && text[j] == '"') ? j : text.size();
			const size_t outer_end = (j < text.size() && text[j] == '"') ? (j + 1) : text.size();
			tag.tokens.push_back(TagToken{
					decode_quoted(text.substr(content_begin, content_end - content_begin)),
					SourceSpan{
							line,
							start_col + static_cast<uint32_t>(content_begin),
							static_cast<uint32_t>(content_end - content_begin),
					},
					SourceSpan{
							line,
							start_col + static_cast<uint32_t>(i),
							static_cast<uint32_t>(outer_end - i),
					},
					true,
			});
			last_separator = '"';
			i = outer_end;
			continue;
		}

		if (next > i)
		{
			tag.tokens.push_back(TagToken{
					std::string(text.substr(i, next - i)),
					SourceSpan{
							line,
							start_col + static_cast<uint32_t>(i),
							static_cast<uint32_t>(next - i),
					},
					SourceSpan{
							line,
							start_col + static_cast<uint32_t>(i),
							static_cast<uint32_t>(next - i),
					},
					false,
			});
			last_separator = separator;
		}
		else if (separator == ',')
		{
			if (last_separator == ',')
			{
				tag.tokens.push_back(TagToken{
						"",
						SourceSpan{},
						SourceSpan{line, start_col + static_cast<uint32_t>(next), 1},
						false,
				});
			}
			else
			{
				last_separator = separator;
			}
		}
		i = next + 1;
	}
}

std::optional<uint16_t> parse_key_from_message(const std::string &message,
																						 const std::string &prefix)
{
	if (message.rfind(prefix, 0) != 0)
		return std::nullopt;
	const auto suffix = message.substr(prefix.size());
	char *end = nullptr;
	long parsed = std::strtol(suffix.c_str(), &end, 10);
	if (suffix.c_str() == end || parsed < 0 || parsed > 0xffffl)
		return std::nullopt;
	return static_cast<uint16_t>(parsed);
}
}

namespace ctrmml_cmd
{
SupplementalChecker::SupplementalChecker(const std::string &text, const std::filesystem::path &base_dir)
		: base_dir_(base_dir)
{
	std::unordered_map<std::string, size_t> tag_lookup;
	std::string current_tag_key;

	std::istringstream stream(text);
	std::string line;
	uint32_t line_number = 1;
	while (std::getline(stream, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		size_t first_nonblank = 0;
		while (first_nonblank < line.size() && is_blank_char(line[first_nonblank]))
			++first_nonblank;

		if (first_nonblank > 0)
		{
			if (current_tag_key.empty())
			{
				++line_number;
				continue;
			}
			if (first_nonblank >= line.size())
			{
				++line_number;
				continue;
			}
			auto lookup = tag_lookup.find(current_tag_key);
			if (lookup != tag_lookup.end())
			{
				tokenize_tag_values(tag_order_[lookup->second],
														std::string_view(line).substr(first_nonblank),
														line_number,
														static_cast<uint32_t>(first_nonblank + 1));
			}
			++line_number;
			continue;
		}

		if (line.empty() || line[0] == ';')
		{
			++line_number;
			continue;
		}

		if (line[0] == '#' || line[0] == '@')
		{
			size_t key_end = 1;
			while (key_end < line.size() && !is_blank_char(line[key_end]))
				++key_end;
			std::string key = line.substr(0, key_end);
			auto it = tag_lookup.find(key);
			if (it == tag_lookup.end())
			{
				tag_lookup.emplace(key, tag_order_.size());
				tag_order_.push_back(TagDefinition{
						key,
						SourceSpan{line_number, 1, static_cast<uint32_t>(key.size())},
						{},
				});
				it = tag_lookup.find(key);
			}
			current_tag_key = key;

			size_t content_start = key_end;
			while (content_start < line.size() && is_blank_char(line[content_start]))
				++content_start;
			if (content_start < line.size())
			{
				tokenize_tag_values(tag_order_[it->second],
														std::string_view(line).substr(content_start),
														line_number,
														static_cast<uint32_t>(content_start + 1));
			}
			++line_number;
			continue;
		}

		if (is_track_id_start(line[0]))
			current_tag_key.clear();
		else
			current_tag_key.clear();

		++line_number;
	}
}

const TagDefinition *SupplementalChecker::find_tag(const std::string &key) const
{
	for (const auto &tag : tag_order_)
	{
		if (tag.key == key)
			return &tag;
	}
	return nullptr;
}

std::vector<CheckMessage> SupplementalChecker::collect_errors(const std::string &display_name) const
{
	std::vector<CheckMessage> errors;
	std::unordered_map<uint16_t, bool> defined_instruments;
	defined_instruments.emplace(0, true);

	for (const auto &tag : tag_order_)
	{
		uint16_t instrument_id = 0;
		if (!is_instrument_key(tag.key, instrument_id))
			continue;
		if (tag.tokens.empty())
			continue;

		const auto &type_token = tag.tokens.front();
		const auto type_span = preferred_span(&type_token, tag.key_span);
		const std::string &type = type_token.value;

		if (type == "fm")
		{
			if (tag.tokens.size() < 43)
			{
				errors.push_back(make_message(display_name,
																type_span,
																"error: not enough parameters for fm instrument @" + std::to_string(instrument_id),
																"conversion_error"));
				continue;
			}
			defined_instruments[instrument_id] = true;
			continue;
		}

		if (type == "2op")
		{
			if (tag.tokens.size() < 7)
			{
				errors.push_back(make_message(display_name,
																type_span,
																"error: not enough parameters for 2op fm instrument @" + std::to_string(instrument_id),
																"conversion_error"));
				continue;
			}

			int base_instrument = std::strtol(tag.tokens[1].value.c_str(), nullptr, 10);
			if (defined_instruments.find(static_cast<uint16_t>(base_instrument)) == defined_instruments.end())
			{
				errors.push_back(make_message(
						display_name,
						preferred_span(&tag.tokens[1], tag.key_span),
						"2op ins @" + std::to_string(instrument_id) + " is referencing instrument @" + std::to_string(base_instrument) + " which does not exist",
						"conversion_error"));
				continue;
			}

			defined_instruments[instrument_id] = true;
			continue;
		}

		if (type == "psg")
		{
			defined_instruments[instrument_id] = true;
			continue;
		}

		if (type == "pcm")
		{
			if (tag.tokens.size() < 2 || tag.tokens[1].value.empty())
			{
				const TagToken *path_token = tag.tokens.size() >= 2 ? &tag.tokens[1] : nullptr;
				errors.push_back(make_message(display_name,
																preferred_span(path_token, type_span),
																"missing pcm sample",
																"pcm_missing"));
				continue;
			}

			const auto &path_token = tag.tokens[1];
			std::filesystem::path sample_path(path_token.value);
			if (!sample_path.is_absolute())
				sample_path = base_dir_ / sample_path;

			std::error_code ec;
			if (!std::filesystem::exists(sample_path, ec))
			{
				errors.push_back(make_message(display_name,
																preferred_span(&path_token, type_span),
																"missing pcm sample: " + path_token.value,
																"pcm_missing"));
				continue;
			}

			defined_instruments[instrument_id] = true;
			continue;
		}

		errors.push_back(make_message(display_name,
														type_span,
														"unknown envelope type " + type,
														"conversion_error"));
	}

	return errors;
}

std::optional<CheckMessage> SupplementalChecker::try_map_locationless_error(const std::string &display_name,
																																					 const std::string &message,
																																					 const std::string &code) const
{
	if (auto key = parse_key_from_message(message, "error: not enough parameters for fm instrument @"))
	{
		if (const auto *tag = find_tag("@" + std::to_string(*key)))
			return make_message(display_name, tag->key_span, message, code);
	}

	if (auto key = parse_key_from_message(message, "error: not enough parameters for 2op fm instrument @"))
	{
		if (const auto *tag = find_tag("@" + std::to_string(*key)))
			return make_message(display_name, tag->key_span, message, code);
	}

	if (message.rfind("2op ins @", 0) == 0)
	{
		auto ref_pos = message.find(" is referencing instrument @");
		if (ref_pos != std::string::npos)
		{
			auto id_text = message.substr(9, ref_pos - 9);
			char *end = nullptr;
			long id = std::strtol(id_text.c_str(), &end, 10);
			if (id_text.c_str() != end && id >= 0 && id <= 0xffffl)
			{
				if (const auto *tag = find_tag("@" + std::to_string(id)))
				{
					const TagToken *token = tag->tokens.size() >= 2 ? &tag->tokens[1] : nullptr;
					return make_message(display_name, preferred_span(token, tag->key_span), message, code);
				}
			}
		}
	}

	if (message == "Incomplete sample definition")
	{
		for (const auto &tag : tag_order_)
		{
			uint16_t instrument_id = 0;
			if (!is_instrument_key(tag.key, instrument_id))
				continue;
			if (!tag.tokens.empty() && tag.tokens.front().value == "pcm")
			{
				const TagToken *path_token = tag.tokens.size() >= 2 ? &tag.tokens[1] : nullptr;
				return make_message(display_name,
														preferred_span(path_token, tag.key_span),
														"missing pcm sample",
														"pcm_missing");
			}
		}
	}

	const std::string not_found_suffix = " not found";
	if (message.size() > not_found_suffix.size()
			&& message.compare(message.size() - not_found_suffix.size(),
												 not_found_suffix.size(),
												 not_found_suffix) == 0)
	{
		std::string path = message.substr(0, message.size() - not_found_suffix.size());
		for (const auto &tag : tag_order_)
		{
			uint16_t instrument_id = 0;
			if (!is_instrument_key(tag.key, instrument_id))
				continue;
			if (tag.tokens.size() >= 2 && tag.tokens.front().value == "pcm" && tag.tokens[1].value == path)
			{
				return make_message(display_name,
														preferred_span(&tag.tokens[1], tag.key_span),
														"missing pcm sample: " + path,
														"pcm_missing");
			}
		}
	}

	return std::nullopt;
}
}
