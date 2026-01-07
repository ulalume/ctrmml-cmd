#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ctrmml_cmd
{
	struct CheckMessage
	{
		std::string message;
		std::string path;
		uint32_t line = 0;
		uint32_t col = 0;
		std::string code;
		std::string raw;
	};

	struct CheckReport
	{
		std::vector<CheckMessage> errors;
		std::vector<CheckMessage> warnings;

		bool ok() const { return errors.empty(); }
	};

	struct CheckResult
	{
		bool ok;
		std::string error;
	};

	CheckReport check_file_report(const std::string &path);
	CheckReport check_text_report(const std::string &text,
																const std::string &base_dir,
																const std::string &display_name);
	std::string check_report_json(const CheckReport &report);

	CheckResult check_file(const std::string &path);
	CheckResult check_text(const std::string &text,
												 const std::string &base_dir,
												 const std::string &display_name);
}
