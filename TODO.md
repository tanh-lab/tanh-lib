TODOs

- With RT-San fix the tests that throw exceptions (SmartHandle.ThrowsOnNonexistentParameter, StateTests.HandleNonExistentKey)
- Properly disable HardwareTests on plattforms that are not supported
- The ModulationMatrix shall also be able to modulate bools or ints or doubles, modulation shall be able to be normalized
- Check that RCU still works after setting the min_active_period to current_period in the cleanup_safe_versions method (no more to UINT64_MAX)