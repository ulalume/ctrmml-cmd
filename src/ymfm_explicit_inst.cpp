// Explicit template instantiation for ymfm symbols used by
// YmfmYm2612Device::generate_masked().
//
// Why: fm_engine_base<...>::output() is a template member function defined
// in ymfm_fm.ipp. When ymfm_opn.cpp includes the .ipp, it implicitly
// instantiates the template — but optimized GCC/MSVC builds (Release on
// Linux/Windows) inline every call site within ymfm_opn.cpp and never emit
// an external symbol. Other TUs that reference output() then fail to link.
//
// Forcing an explicit instantiation here ensures the symbol has external
// linkage. This file is built as part of ymfm-core, so the related
// non-template helpers in ymfm_opn.cpp resolve at link time.

#include <ymfm_opn.h>
#include <ymfm_fm.ipp>

namespace ymfm {

template void fm_engine_base<opn_registers_base<true>>::output(
	ymfm_output<2> &output, uint32_t rshift, int32_t clipmax, uint32_t chanmask) const;

} // namespace ymfm
