#pragma once

#include <exception>
#include <new>

#include "input.h"

//! The text of the exception being handled, to report it: what() of an
//! InputError, which holds its source location, or of another
//! std::exception, and `out_of_memory` for std::bad_alloc. Call it in a catch
//! block; the text is valid until the block ends.
inline const char *current_exception_text(const char *out_of_memory = "not enough memory")
{
	try
	{
		throw;
	}
	catch (InputError &e)
	{
		return e.what();
	}
	catch (std::bad_alloc &)
	{
		return out_of_memory;
	}
	catch (std::exception &e)
	{
		return e.what();
	}
	catch (...)
	{
		return "unknown exception";
	}
}
