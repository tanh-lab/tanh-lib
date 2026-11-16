#include "tanh/parameters/ParameterExceptions.h"

#include <tanh/core/AbiNamespace.h>

namespace thl::inline THL_ABI_NAMESPACE {

// The key functions: the vtables and typeinfo live in tanh::Parameters only, so a
// catch in one component sees what another one threw.
ParameterNotFoundException::~ParameterNotFoundException() = default;
ParameterTypeMismatchException::~ParameterTypeMismatchException() = default;

}  // namespace thl::inline THL_ABI_NAMESPACE
