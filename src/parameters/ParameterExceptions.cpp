#include "tanh/parameters/ParameterExceptions.h"

namespace thl {

// The key functions: the vtables and typeinfo live in tanh::Parameters only, so a
// catch in one component sees what another one threw.
ParameterNotFoundException::~ParameterNotFoundException() = default;
ParameterTypeMismatchException::~ParameterTypeMismatchException() = default;

}  // namespace thl
