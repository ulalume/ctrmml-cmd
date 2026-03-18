#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ctrmml_cmd.h"

namespace ctrmml_cmd
{
class SupplementalChecker
{
public:
	struct SourceSpan
	{
		uint32_t line = 0;
		uint32_t col = 0;
		uint32_t length = 0;
	};

	struct TagToken
	{
		std::string value;
		SourceSpan span;
		SourceSpan outer_span;
		bool quoted = false;
	};

	struct TagDefinition
	{
		std::string key;
		SourceSpan key_span;
		std::vector<TagToken> tokens;
	};

	SupplementalChecker(const std::string &text, const std::filesystem::path &base_dir);

	std::vector<CheckMessage> collect_errors(const std::string &display_name) const;
	std::optional<CheckMessage> try_map_locationless_error(const std::string &display_name,
																									 const std::string &message,
																									 const std::string &code) const;

private:
	std::filesystem::path base_dir_;
	std::vector<TagDefinition> tag_order_;

	const TagDefinition *find_tag(const std::string &key) const;
};
}
